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

static int
ov_write_file(const char *path, const uint8_t *data, uint64_t size, int mode)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int err;

	ov_mkdirs_parent(path);
	err = vnode_open(path, O_CREAT | O_TRUNC | FWRITE, mode & 07777, 0, &vp, ctx);
	if (err != 0) {
		return -1;
	}
	(void)vnode_setsize(vp, 0, 0, ctx);
	uint64_t off = 0;
	while (off < size) {
		size_t chunk = (size_t)MIN((uint64_t)PAGE_SIZE, size - off);
		int resid = 0;
		err = vn_rdwr(UIO_WRITE, vp, (caddr_t)(data + off), (int)chunk, (off_t)off,
		    UIO_SYSSPACE, IO_UNIT | IO_SYNC, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx));
		if (err != 0 || resid != 0) {
			vnode_close(vp, FWRITE, ctx);
			return -1;
		}
		off += chunk;
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

static void
ov_apply(void)
{
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

	char path[OV_MAXPATH], link[OV_MAXPATH];
	unsigned files = 0, dirs = 0, links = 0, errs = 0;
	uint64_t pos = 0;

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

		if (type == '5') {
			if (ov_mkdir(path, mode ? mode : 0755) == 0) {
				dirs++;
			} else {
				errs++;
			}
		} else if (type == '2') {
			strlcpy(link, (const char *)(hdr + 157), sizeof(link));
			if (ov_symlink(path, link) == 0) {
				links++;
			} else {
				errs++;
			}
		} else if (type == '0' || type == 0 || type == '7') {
			if (ov_write_file(path, bytes + data, fsize, mode ? mode : 0644) == 0) {
				files++;
			} else {
				errs++;
				ov_log("%s failed\n", path);
			}
		}
		pos = next;
	}

	/* stamp it so the next boot with the same payload skips all of this */
	char txt[20];
	snprintf(txt, sizeof(txt), "%016llx\n", (unsigned long long)h);
	if (ov_mkdir("/private/var/local", 0777) == 0) {
		(void)ov_write_file(OV_STAMP, (const uint8_t *)txt, strlen(txt), 0644);
	}
	ov_log("%u files, %u dirs, %u links, %u errors (hash %016llx)\n", files, dirs, links,
	    errs, (unsigned long long)h);
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

