/*
 * Install a file tree into the iOS root volume at boot.
 *
 * macOS/iOS pick a Metal driver by loading the bundle named in the GPU
 * service's MetalPluginName from /System/Library/Extensions. Our root image
 * was built without SweetMetal.bundle, and re-flashing 11.3 GB to add three
 * files is not acceptable. So the bundle travels inside the payload as a
 * ustar blob on /chosen ("sweet-overlay", written by mkpayload.py --overlay)
 * and is unpacked here, after the root is mounted and before launchd asks for
 * a Metal device.
 *
 * Entries are applied in archive order. Directories are created as needed,
 * files are written whole, symlinks are recreated. A stamp file records the
 * FNV-1a hash of the blob so a second boot with the same payload does nothing.
 */
#if defined(ARM64_BOARD_CONFIG_GUACAMOLE)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/vnode_if.h>
#include <sys/vnode_internal.h>	/* vnode_setsize: not in the public vnode.h */
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

/* The arm64 kernel build has -Werror=implicit-function-declaration and neither
 * of these is reachable from the headers above; declare them exactly as XNU
 * defines them (sweet_a6xx_host.c declares thread_terminate the same way). */
extern kern_return_t thread_terminate(thread_t);

extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int vprintf(const char *fmt, va_list ap);

#define OV_STAMP	"/private/var/local/.sweet-overlay"
#define OV_MAXPATH	320

/*
 * The archive stores paths relative to /, and the only entries we care about
 * live under this prefix. Variant 1 re-roots just the bundle into
 * /private/var/local: iOS always allows writes there (it is where the stamp
 * and the shader caches go), whereas /System/Library/Extensions is refused on
 * some boots. Metal reaches either copy because the path it builds is
 * "/System/Library/Extensions/<MetalPluginName>.bundle", and that name is a
 * boot-arg: "../../../private/var/local/SweetMetal" normalises to the second
 * location (SweetAccelerator.cpp).
 */
#define OV_SRC_PREFIX	"System/Library/Extensions/"
#define OV_VAR_ROOT	"/private/var/local/"
/* What MetalPluginName becomes when only the /private/var copy landed:
 * "/System/Library/Extensions/" + this + ".bundle" normalises to that copy. */
#define OV_VAR_PLUGIN	"../../../private/var/local/SweetMetal"

static void
ov_log(const char *fmt, ...)
{
	va_list ap;
	printf("SWEET OVERLAY: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static uint64_t
ov_octal(const uint8_t *p, unsigned len)
{
	uint64_t v = 0;
	for (unsigned i = 0; i < len; i++) {
		if (p[i] >= '0' && p[i] <= '7') {
			v = (v << 3) | (uint64_t)(p[i] - '0');
		} else if (p[i] == 0 || p[i] == ' ') {
			break;
		}
	}
	return v;
}

static int
ov_mkdir(const char *path, int mode)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;

	if (vnode_lookup(path, 0, &vp, ctx) == 0) {
		vnode_put(vp);
		return 0;		/* already there */
	}
	/* parent */
	char parent[OV_MAXPATH];
	strlcpy(parent, path, sizeof(parent));
	char *slash = strrchr(parent, '/');
	if (slash == NULL || slash == parent) {
		return -1;
	}
	*slash = 0;
	if (ov_mkdir(parent, 0755) != 0) {
		return -1;
	}

	struct componentname cn;
	struct vnode_attr va;
	vnode_t dvp = NULLVP;

	if (vnode_lookup(parent, 0, &dvp, ctx) != 0) {
		return -1;
	}
	bzero(&cn, sizeof(cn));
	cn.cn_nameptr = slash + 1;
	cn.cn_namelen = (uint32_t)strlen(slash + 1);
	VATTR_INIT(&va);
	VATTR_SET(&va, va_type, VDIR);
	VATTR_SET(&va, va_mode, mode & 07777);
	int err = VNOP_MKDIR(dvp, &cn, &va, &vp, ctx);
	if (err == 0 && vp != NULLVP) {
		vnode_put(vp);
	}
	vnode_put(dvp);
	return err == 0 ? 0 : -1;
}

static void
ov_mkdirs_parent(const char *path)
{
	char parent[OV_MAXPATH];
	strlcpy(parent, path, sizeof(parent));
	char *slash = strrchr(parent, '/');
	if (slash == NULL || slash == parent) {
		return;
	}
	*slash = 0;
	(void)ov_mkdir(parent, 0755);
}

/* Kernel vnode lookups need a leading slash; ustar names never have one. */
static const char *
ov_abs(const char *path, char *buf, size_t bufsz)
{
	if (path[0] == '/') {
		return path;
	}
	snprintf(buf, bufsz, "/%s", path);
	return buf;
}

static int ov_last_errno;

/* A stale protected vnode or an immutable/append-only flag makes even root's
 * O_CREAT fail; clearing the flags is the only thing an in-kernel writer can do
 * about that, and it is exactly what the reference project's file service
 * implies (it owns the volume and never sets them). */
static int
ov_clear_flags(const char *path)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	struct vnode_attr va;
	int err;

	if (vnode_lookup(path, 0, &vp, ctx) != 0) {
		return -1;
	}
	VATTR_INIT(&va);
	VATTR_SET(&va, va_flags, 0);
	err = vnode_setattr(vp, &va, ctx);
	vnode_put(vp);
	return err == 0 ? 0 : -1;
}

static int
ov_write_file(const char *path, const uint8_t *data, uint64_t size, int mode)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int err;
	char abuf[OV_MAXPATH];
	const char *apath = ov_abs(path, abuf, sizeof(abuf));

	path = apath;
	ov_mkdirs_parent(path);
	err = vnode_open(path, O_CREAT | O_TRUNC | FWRITE, mode & 07777, 0, &vp, ctx);
	if (err != 0 && ov_clear_flags(path) == 0) {
		ov_log("cleared flags on %s, retrying\n", path);
		err = vnode_open(path, O_CREAT | O_TRUNC | FWRITE, mode & 07777, 0, &vp, ctx);
	}
	if (err != 0) {
		ov_last_errno = err;
		ov_log("open %s failed err %d\n", path, err);
		return -1;
	}
	int seterr = vnode_setsize(vp, 0, 0, ctx);
	if (seterr != 0) {
		ov_log("setsize %s failed err %d\n", path, seterr);
	}
	uint64_t off = 0;
	while (off < size) {
		size_t chunk = (size_t)MIN((uint64_t)PAGE_SIZE, size - off);
		int resid = 0;
		err = vn_rdwr(UIO_WRITE, vp, (caddr_t)(data + off), (int)chunk, (off_t)off,
		    UIO_SYSSPACE, IO_UNIT | IO_SYNC, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx));
		if (err != 0) {
			ov_log("write %s off %llu failed err %d\n", path,
			    (unsigned long long)off, err);
			vnode_close(vp, FWRITE, ctx);
			return -1;
		}
		if (resid == (int)chunk) {
			ov_log("write %s off %llu stalled (resid %d)\n", path,
			    (unsigned long long)off, resid);
			vnode_close(vp, FWRITE, ctx);
			return -1;
		}
		off += (uint64_t)chunk - (uint64_t)resid;
	}
	(void)vnode_close(vp, FWRITE, ctx);

	struct vnode_attr va;
	if (vnode_lookup(path, 0, &vp, ctx) == 0) {
		VATTR_INIT(&va);
		VATTR_SET(&va, va_mode, mode & 07777);
		(void)vnode_setattr(vp, &va, ctx);
		vnode_put(vp);
	}
	return 0;
}

static int
ov_symlink(const char *path, const char *target)
{
	/* This XNU has no vn_symlink(), and the overlay only ships regular files
	 * (the Metal bundle), so a link is logged and skipped instead of failing
	 * the whole injection. */
	ov_log("skip symlink %s -> %s (not supported here)\n", path, target);
	return -1;
}

static uint64_t
ov_hash(const uint8_t *p, unsigned len)
{
	uint64_t h = 14695981039346656037ULL;
	for (unsigned i = 0; i < len; i++) {
		h = (h ^ p[i]) * 1099511628211ULL;
	}
	return h;
}

static int
ov_stamp_matches(uint64_t h)
{
	char want[20];
	snprintf(want, sizeof(want), "%016llx\n", (unsigned long long)h);

	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int match = 0;

	if (vnode_lookup(OV_STAMP, 0, &vp, ctx) != 0) {
		return 0;
	}
	char have[20];
	int resid = 0;
	bzero(have, sizeof(have));
	int err = vn_rdwr(UIO_READ, vp, (caddr_t)have, 17, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	vnode_put(vp);
	if (err == 0 && resid == 0 && memcmp(have, want, 17) == 0) {
		match = 1;
	}
	return match;
}

/*
 * This hook runs from the UFS probe, which is earlier than the rootrw=1 remount,
 * so a lookup of /System/Library succeeds while the mount is still read-only and
 * every write fails with EROFS. Poll a real write to the same mount instead.
 */
static int
ov_wait_writable(void)
{
	static const uint8_t probe[1] = { 'x' };
	/* /private/var/local may not exist yet: creating it is itself the first
	 * writability test, and probing a missing directory always fails ENOENT. */
	(void)ov_mkdir("/private/var", 0755);
	(void)ov_mkdir("/private/var/local", 0777);
	/* Fail fast: with sweetovl_halt=1 the first failure is answered by a panic,
	 * so a photo names the errno within a couple of seconds instead of a minute. */
	uint32_t hf = 0;
	int fast = PE_parse_boot_argn("sweetovl_halt", &hf, sizeof(hf)) && hf;
	for (int i = 0; i < (fast ? 20 : 240); i++) {
		if (ov_write_file("/private/var/local/.ovprobe", probe, 1, 0644) == 0) {
			ov_log("root is writable after %d ms\n", i * 500);
			return 0;
		}
		if (i % 20 == 0) {
			ov_log("still not writable after %d ms (err %d)\n", i * 500, ov_last_errno);
		}
		delay(500 * 1000);
	}
	ov_log("root never became writable (err %d)\n", ov_last_errno);
	return -1;
}

static int ov_variant;		/* 0: /System layout, 1: /private/var/local */
static unsigned ov_nfiles, ov_ndirs, ov_nlinks, ov_nerrs;

/* One pass over the ustar blob. Variant 0 installs the archive as it was
 * written (/System/Library/Extensions/...); variant 1 skips everything above
 * the bundle and re-roots the bundle into /private/var/local. */
static void
ov_walk(const uint8_t *bytes, uint64_t size)
{
	char path[OV_MAXPATH], link[OV_MAXPATH];
	uint64_t pos = 0;

	ov_nfiles = ov_ndirs = ov_nlinks = ov_nerrs = 0;

	while (pos + 512 <= (uint64_t)size) {
		const uint8_t *hdr = bytes + pos;
		int zero = 1;
		for (unsigned i = 0; i < 512; i++) {
			if (hdr[i] != 0) {
				zero = 0;
				break;
			}
		}
		if (zero) {
			break;		/* end of archive */
		}
		uint64_t fsize = ov_octal(hdr + 124, 12);
		int mode = (int)(ov_octal(hdr + 100, 8) & 07777);
		char type = (char)hdr[156];
		uint64_t data = pos + 512;
		uint64_t next = data + ((fsize + 511) & ~511ULL);

		/* name, then the "prefix" field when the name is a ustar split one */
		strlcpy(path, (const char *)hdr, sizeof(path));
		char prefix[156];
		strlcpy(prefix, (const char *)(hdr + 345), sizeof(prefix));
		if (prefix[0] != 0) {
			char tmp[OV_MAXPATH];
			snprintf(tmp, sizeof(tmp), "%s/%s", prefix, path);
			strlcpy(path, tmp, sizeof(path));
		}

		char full[OV_MAXPATH];
		const size_t plen = sizeof(OV_SRC_PREFIX) - 1;
		if (ov_variant == 1) {
			if (strncmp(path, OV_SRC_PREFIX, plen) != 0) {
				pos = next;
				continue;	/* only the bundle goes to /private/var */
			}
			snprintf(full, sizeof(full), "%s%s", OV_VAR_ROOT, path + plen);
		} else {
			snprintf(full, sizeof(full), "/%s", path);
		}
		/* ustar directory names carry a trailing slash; the vnode calls do not */
		size_t flen = strlen(full);
		while (flen > 1 && full[flen - 1] == '/') {
			full[--flen] = 0;
		}

		if (type == '5') {
			if (ov_mkdir(full, mode ? mode : 0755) == 0) {
				ov_ndirs++;
			} else {
				ov_nerrs++;
			}
		} else if (type == '2') {
			strlcpy(link, (const char *)(hdr + 157), sizeof(link));
			if (ov_symlink(full, link) == 0) {
				ov_nlinks++;
			} else {
				ov_nerrs++;
			}
		} else if (type == '0' || type == 0 || type == '7') {
			if (ov_write_file(full, bytes + data, fsize, mode ? mode : 0644) == 0) {
				ov_nfiles++;
			} else {
				ov_nerrs++;
				ov_log("%s failed\n", full);
			}
		}
		pos = next;
	}
}

static void
ov_apply(void)
{
	/* Try anyway even if the probe never succeeded: the probe tests one path,
	 * and a failure there says nothing about the other destination. */
	(void)ov_wait_writable();

	DTEntry chosen;
	void const *blob = NULL;
	unsigned size = 0;

	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "sweet-overlay", &blob, &size) != kSuccess ||
	    blob == NULL || size < 512) {
		return;			/* no overlay in this payload */
	}
	const uint8_t *bytes = (const uint8_t *)blob;
	uint64_t h = ov_hash(bytes, size);
	if (ov_stamp_matches(h)) {
		ov_log("stamp %016llx already applied\n", (unsigned long long)h);
		return;
	}

	ov_variant = 0;
	ov_walk(bytes, size);
	unsigned sysf = ov_nfiles, sysd = ov_ndirs, syse = ov_nerrs;
	ov_log("System: %u files, %u dirs, %u errors\n", sysf, sysd, syse);

	ov_variant = 1;
	ov_walk(bytes, size);
	unsigned varf = ov_nfiles, vard = ov_ndirs, vare = ov_nerrs;
	ov_log("/private/var/local: %u files, %u dirs, %u errors\n", varf, vard, vare);

	/* Only the fallback copy is complete, so point Metal at it. */
	if (syse > 0 && vare == 0 && varf > 0) {
		extern void sweet_metal_plugin_set(const char *name);
		ov_log("redirecting Metal to %s\n", OV_VAR_PLUGIN);
		sweet_metal_plugin_set(OV_VAR_PLUGIN);
	}

	/* stamp it so the next boot with the same payload skips all of this */
	char txt[20];
	snprintf(txt, sizeof(txt), "%016llx\n", (unsigned long long)h);
	if (syse == 0 && vare == 0) {
		(void)ov_write_file(OV_STAMP, (const uint8_t *)txt, strlen(txt), 0644);
	}
	{
		/* sweetovl_halt=1: freeze here so one photo shows whether the
		 * bundle reached the root filesystem and where. */
		uint32_t hh = 0;
		if (PE_parse_boot_argn("sweetovl_halt", &hh, sizeof(hh)) && hh) {
			panic("SWEET OVL DONE: sys %u/%u var %u/%u hash %016llx",
			    sysf, syse, varf, vare, (unsigned long long)h);
		}
	}
}

static void
ov_thread(void *arg __unused, wait_result_t wr __unused)
{
	/* The root volume is mounted by the time this hook runs, but give the
	 * mount a moment to settle before touching the tree. */
	for (int i = 0; i < 40; i++) {
		delay(250 * 1000);
		vfs_context_t ctx = vfs_context_kernel();
		vnode_t vp = NULLVP;
		if (vnode_lookup("/System/Library", 0, &vp, ctx) == 0) {
			vnode_put(vp);
			break;
		}
	}
	ov_apply();
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_overlay_start(void)
{
	thread_t thread;
	if (kernel_thread_start(ov_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

#endif /* ARM64_BOARD_CONFIG_GUACAMOLE */

