/*
 * Kernel environment for the Adreno 640 core in sweet_a6xx.c.
 *
 * The core is the one that was proven on this phone in bare metal (shim-rmx):
 * CX/GX power, GMU firmware, HFI, SMMU, SQE, CP ring, zap and the blit stream.
 * It only needs eight environment calls, and this file is that environment.
 *
 * Boot arguments:
 *   sweeta6xx=0  (default) do nothing at all
 *   sweeta6xx=1  power the GPU, boot the GMU, bring the CP up WITHOUT the zap
 *                shader: no SCM call is made in this stage, so a TrustZone
 *                surprise cannot cost the run
 *   sweeta6xx=2  additionally load and authenticate the zap shader (PAS 13)
 *   sweeta6xx=3  2 plus a CP self-test: the ring asks CP_MEM_WRITE to put a
 *                magic value into the pool, the CPU reads it back
 *   sweetzap=0x...  /reserved-memory/pil_gpu_region as the bootloader actually
 *                placed it (shim-rmx reads the live tree; ABL rebases the whole
 *                pil_* group by 4 MiB, so the static DTB value is wrong)
 *
 * Firmware comes from the device tree as /chosen "gpufw,<name>", written by
 * tools/mkpayload.py --gpufw DIR.
 */
#if defined(ARM64_BOARD_CONFIG_GUACAMOLE)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/debug.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include "sweet_a6xx.h"

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern boolean_t pmap_valid_address(uint64_t pa);
extern vm_map_address_t phystokv(uint64_t pa);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern kern_return_t thread_terminate(thread_t);
extern int hz;		/* kern/clock.c; tsleep ticks, not in every header set */
extern int vprintf(const char *fmt, va_list ap);

void sweet_a6xx_init(void);

static struct a6_gpu gpu;
static vm_offset_t pool_kva;
static volatile bool gpu_ready;
static int resolved_stage;	/* sweeta6xx (or the gpu= alias) resolved in init */
static LCK_GRP_DECLARE(sweet_a6xx_grp, "sweet_a6xx");
static LCK_MTX_DECLARE(sweet_a6xx_lock, &sweet_a6xx_grp);

/* ---- environment ---- */

#define GPU_WIN		0x200000ULL
#define GPU_MAPS	24

static struct {
	vm_offset_t va;
	uint64_t win;
} gpu_map[GPU_MAPS];

/* Device windows: 2 MB, kept for good (there are only a handful of blocks). */
static volatile uint32_t *
gpu_io(uint64_t pa)
{
	uint64_t win = pa & ~(GPU_WIN - 1);
	for (unsigned i = 0; i < GPU_MAPS; i++) {
		if (gpu_map[i].va && gpu_map[i].win == win) {
			return (volatile uint32_t *)(gpu_map[i].va + (pa - win));
		}
		if (!gpu_map[i].va) {
			gpu_map[i].va = ml_io_map_unmappable((vm_offset_t)win, GPU_WIN, VM_WIMG_IO);
			gpu_map[i].win = win;
			return gpu_map[i].va ? (volatile uint32_t *)(gpu_map[i].va + (pa - win)) : NULL;
		}
	}
	panic("sweet_a6xx: out of device windows at 0x%llx", pa);
}

static bool
in_pool(uint64_t pa, size_t len)
{
	return pool_kva && pa >= gpu.pool_pa && pa + len <= gpu.pool_pa + A6_POOL_SIZE;
}

uint32_t
a6_rd(uint64_t pa)
{
	uint32_t v;
	if (in_pool(pa, 4)) {
		(void)a6_read(pa, &v, 4);
		return v;
	}
	v = *gpu_io(pa);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

void
a6_wr(uint64_t pa, uint32_t v)
{
	if (in_pool(pa, 4)) {
		(void)a6_write(pa, &v, 4);
		return;
	}
	__asm__ volatile ("dmb oshst" ::: "memory");
	*gpu_io(pa) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

/* Pool and other kernel RAM: through the physmap, cleaned to the point of
 * coherency. MMIO: words. */
int
a6_write(uint64_t pa, const void *buf, size_t len)
{
	if (in_pool(pa, len)) {
		memcpy((void *)(pool_kva + (pa - gpu.pool_pa)), buf, len);
		flush_dcache64((addr64_t)pa, (unsigned)len, TRUE);
		return 0;
	}
	if (pmap_valid_address(pa)) {
		memcpy((void *)phystokv(pa), buf, len);
		flush_dcache64((addr64_t)pa, (unsigned)len, TRUE);
		return 0;
	}
	const uint32_t *w = buf;
	for (size_t i = 0; i < len / 4; i++) {
		a6_wr(pa + 4 * i, w[i]);
	}
	return 0;
}

int
a6_read(uint64_t pa, void *buf, size_t len)
{
	if (in_pool(pa, len)) {
		flush_dcache64((addr64_t)pa, (unsigned)len, TRUE);
		memcpy(buf, (const void *)(pool_kva + (pa - gpu.pool_pa)), len);
		return 0;
	}
	uint32_t *w = buf;
	for (size_t i = 0; i < len / 4; i++) {
		w[i] = a6_rd(pa + 4 * i);
	}
	return 0;
}

void
a6_delay_ms(unsigned ms)
{
	delay((int)ms * 1000);
}

void
a6_log(const char *fmt, ...)
{
	va_list ap;
	printf("SWEET A6XX: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/*
 * SMC64 (SMCCC) with x0..x7 in and out. The shim does this from bare metal and
 * it works; the kernel path is unproven, which is why stage 1 does not use it.
 */
void
a6_smc(uint64_t x[8])
{
	register uint64_t x0 __asm__("x0") = x[0];
	register uint64_t x1 __asm__("x1") = x[1];
	register uint64_t x2 __asm__("x2") = x[2];
	register uint64_t x3 __asm__("x3") = x[3];
	register uint64_t x4 __asm__("x4") = x[4];
	register uint64_t x5 __asm__("x5") = x[5];
	register uint64_t x6 __asm__("x6") = x[6];
	register uint64_t x7 __asm__("x7") = x[7];

	__asm__ __volatile__("smc #0"
	    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
	      "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
	    :
	    : "memory");
	x[0] = x0; x[1] = x1; x[2] = x2; x[3] = x3;
	x[4] = x4; x[5] = x5; x[6] = x6; x[7] = x7;
}

const uint8_t *
a6_firmware(const char *name, uint32_t *len)
{
	DTEntry chosen;
	void const *data;
	unsigned size;
	char prop[40];

	snprintf(prop, sizeof(prop), "gpufw,%s", name);
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, prop, &data, &size) != kSuccess) {
		a6_log("no firmware %s in the device tree (mkpayload.py --gpufw)\n", name);
		return NULL;
	}
	*len = size;
	return data;
}

/* ---- CP self-test: the GPU must write the value, the CPU must read it ---- */

#define A6_TEST_MAGIC	0xcafef00dU

static void
a6_self_test(struct a6_gpu *g)
{
	uint64_t dst = a6_pool_pa(g, A6_OFF_SCRATCH + 0x100);
	uint32_t zero = 0, got;
	uint32_t dw[8];
	uint32_t n = 0;

	(void)a6_write(dst, &zero, sizeof(zero));
	dw[n++] = a6_pkt7(A6_CP_MEM_WRITE, 3);	/* 2 address dwords + 1 value */
	dw[n++] = (uint32_t)dst;
	dw[n++] = (uint32_t)(dst >> 32);
	dw[n++] = A6_TEST_MAGIC;
	int rc = a6_ring(g, dw, n, "ktest", 500);
	got = a6_rd(dst);
	a6_log("ktest: rc %d ring -> %x (want %x)%s\n", rc, got, A6_TEST_MAGIC,
	    got == A6_TEST_MAGIC ? "  *** GPU EXECUTED OUR COMMANDS ***" : "");
}

/* ---- bring-up ---- */

/*
 * Watchdog. Bring-up runs on its own kernel thread, so when it wedges (an
 * MMIO access that never returns, a GMU that never acks) it prints nothing
 * more and the rest of the boot carries on until the GUI paints over the
 * console: the stall is invisible, and every bisect costs a flash cycle.
 * This thread turns that into a panic. A panic stops every CPU and draws its
 * message over everything, so one photo of the panel names the step.
 * sweeta6xx_wd=<seconds>, default 45, 0 = disabled.
 */
static int a6_wd_chan;

static void
a6_watchdog_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t secs = 45;

	if (!PE_parse_boot_argn("sweeta6xx_wd", &secs, sizeof(secs))) {
		secs = 45;
	}
	if (secs) {
		a6_log("watchdog: %u s for bring-up\n", secs);
		(void)tsleep(&a6_wd_chan, PZERO, "a6watchdog", hz * (int)secs);
		if (!a6_core_done) {
			panic("SWEET A6XX WATCHDOG: %u s, phase %d, step '%s', stage %d",
			    secs, a6_core_phase, a6_core_step, resolved_stage);
		}
		a6_log("watchdog: bring-up finished in time, done\n");
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static void
gpu_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t t0 = mach_absolute_time(), ns;
	int stage = resolved_stage;

	a6_mark("thread:enter");
	lck_mtx_lock(&sweet_a6xx_lock);
	int rc = a6_power_up(&gpu);
	if (!rc) {
		a6_unmap_high(&gpu);
		gpu_ready = true;
		if (stage >= 3) {
			a6_self_test(&gpu);
		}
	}
	lck_mtx_unlock(&sweet_a6xx_lock);
	a6_core_done = 1;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	a6_log("bring-up stage %d %s in %llu ms (pool 0x%llx, zap %s)\n", stage,
	    rc ? "FAILED" : "done, GPU ready", ns / 1000000, gpu.pool_pa,
	    stage >= 2 ? "on" : "off");
	/* One fixed line, so a single photo of the froze panel is enough evidence. */
	a6_log("RESULT stage=%d ok=%d ready=%d cp=%d\n", stage, rc == 0, (int)gpu_ready,
	    (int)a6_cp_running(&gpu));
	{
		int halt = 0;
		if (PE_parse_boot_argn("sweeta6xx_halt", &halt, sizeof(halt)) && halt) {
			a6_log("halting (sweeta6xx_halt=1)\n");
			panic("SWEET A6XX RESULT: stage=%d ok=%d ready=%d cp=%d",
			    stage, rc == 0, (int)gpu_ready, (int)a6_cp_running(&gpu));
		}
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_a6xx_init(void)
{
	uint32_t stage = 0;
	uint64_t zap = 0;
	uint32_t alias = 0;
	extern char *PE_boot_args(void);

	a6_mark("init:enter");
	/* Evidence first: prove the hook ran and show what the kernel actually
	 * received. Without this a silent boot-argument mismatch looks exactly
	 * like the driver not being in the kernel at all. */
	int got = PE_parse_boot_argn("sweeta6xx", &stage, sizeof(stage));
	int got_alias = PE_parse_boot_argn("gpu", &alias, sizeof(alias));
	a6_log("init called: sweeta6xx=%u (found %d), gpu=%u (found %d)\n",
	    stage, got, alias, got_alias);
	a6_log("boot-args: [%s]\n", PE_boot_args() ? PE_boot_args() : "(null)");

	if (!stage && alias) {
		stage = alias;		/* ios-sweet/redwood used gpu=1 */
	}
	if (!stage) {
		a6_log("no stage requested, nothing to do\n");
		return;
	}
	if (PE_parse_boot_argn("sweetzap", &zap, sizeof(zap)) && zap) {
		a6_set_zap_region(zap, 0x2000);
	}
	a6_set_zap_enable(stage >= 2);
	resolved_stage = (int)stage;

	/* sweeta6xx_stop=N: stop after phase N (1 CX, 2 GMU boot, 3 HFI, 4 CP),
	 * 0 = stop before the pool work. Absent = run everything. */
	{
		uint32_t stop = 0;
		a6_stop_phase = PE_parse_boot_argn("sweeta6xx_stop", &stop, sizeof(stop)) ?
		    (int)stop : -1;
		a6_log("stop_phase=%d\n", a6_stop_phase);
	}

	vm_offset_t va = 0;
	if (kmem_alloc_contig(kernel_map, &va, A6_POOL_SIZE, PAGE_MASK, 0,
	    (ppnum_t)(atop_64(A6_POOL_ALIGN) - 1),
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		a6_log("POOL FAILED: no %llu bytes of contiguous memory\n", A6_POOL_SIZE);
		a6_log("RESULT stage=%u ok=0 ready=0 cp=0 (pool)\n", stage);
		a6_core_done = 1;
		panic("SWEET A6XX: no %llu bytes of contiguous memory for the GPU pool",
		    A6_POOL_SIZE);
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)va);
	a6_init_state(&gpu, ptoa_64(pn));
	pool_kva = va;
	a6_log("pool %llu bytes at kva %#llx pa %#llx\n", A6_POOL_SIZE, (uint64_t)va,
	    gpu.pool_pa);
	a6_mark("init:pool-ok");
	if (a6_stop_phase == 0) {
		a6_log("*** STOP before bring-up (sweeta6xx_stop=0)\n");
		a6_core_done = 1;
		panic("SWEET A6XX STOP before bring-up (sweeta6xx_stop=0)");
	}
	flush_dcache64((addr64_t)gpu.pool_pa, (unsigned)A6_POOL_SIZE, TRUE);

	thread_t thread;
	if (kernel_thread_start(a6_watchdog_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	if (kernel_thread_start(gpu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

bool
sweet_a6xx_ready(void)
{
	return gpu_ready;
}

/* ======================================================================
 * The layer the display and Metal talk to. Same API the flame/rmx1931 tree
 * exposes (sweet_gpu_{ready,surface_slot,map_surface,present,panel_stride}):
 * iOS hands us a swapped IOSurface's pages, we point GPU virtual addresses at
 * them, and a 2D blit stream copies/scales the frame into the panel memory
 * without the CPU touching a pixel.
 *
 * Panel: OnePlus 7 Pro, 1440x3120, framebuffer at 0x9c000000 (A6_FB_PA).
 * ====================================================================== */

#define SLOTS		4
#define SLOT_IOVA(n)	(0x100000000ULL + 0x1000000ULL * (n))
#define SLOT_MAX	0x1000000ULL
#define SLOT_PAGES	1024
#define IB_OFF		A6_OFF_FREE	/* the present IB lives in the pool */

#define PANEL_W		1440
#define PANEL_H		3120

/* Set by present(): where the fitted frame landed, for the touch mapping. */
int sweet_display_fit_x, sweet_display_fit_w;

static struct {
	uint64_t key;
	uint64_t pages[SLOT_PAGES];
	uint32_t npages, page_size;
	uint64_t used;
} slot[SLOTS];
static uint64_t slot_clock;
static uint64_t stat_frames, stat_clean_abs, stat_gpu_abs;

bool
sweet_gpu_ready(void)
{
	return gpu_ready;
}

int
sweet_gpu_surface_slot(uint64_t key)
{
	for (int i = 0; i < SLOTS; i++) {
		if (slot[i].key == key && slot[i].npages) {
			slot[i].used = ++slot_clock;
			return i;
		}
	}
	return -1;
}

int
sweet_gpu_map_surface(uint64_t key, const uint64_t *pages, uint32_t npages, uint32_t page_size)
{
	if (!gpu_ready || npages > SLOT_PAGES || (uint64_t)npages * page_size > SLOT_MAX) {
		return -1;
	}
	int s = 0;
	for (int i = 1; i < SLOTS; i++) {
		if (slot[i].used < slot[s].used) {
			s = i;
		}
	}
	lck_mtx_lock(&sweet_a6xx_lock);
	int rc = a6_map_pages(&gpu, SLOT_IOVA(s), pages, npages, page_size);
	a6_tlb_flush(&gpu);
	lck_mtx_unlock(&sweet_a6xx_lock);
	if (rc) {
		slot[s].npages = 0;
		return -1;
	}
	memcpy(slot[s].pages, pages, npages * sizeof(uint64_t));
	slot[s].npages = npages;
	slot[s].page_size = page_size;
	slot[s].key = key;
	slot[s].used = ++slot_clock;
	a6_log("surface %llx -> slot %d (%u pages of %u)\n", key, s, npages, page_size);
	return s;
}

/*
 * Blit a mapped surface, scaled to fit and centred, into the panel memory.
 * The CPU rendered that surface through the cache, so clean its pages first.
 * Returns 0 when the GPU finished; the caller falls back to its CPU copy.
 */
int
sweet_gpu_present(int s, uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch,
    uint32_t fb_pitch, uint32_t *out_dx, uint32_t *out_dw)
{
	if (!gpu_ready || s < 0 || s >= SLOTS || !slot[s].npages ||
	    (pitch & 63) || (fb_pitch & 63) || h == 0 || w == 0) {
		return -1;
	}
	uint64_t t0 = mach_absolute_time();
	for (uint32_t i = 0; i < slot[s].npages; i++) {
		flush_dcache64((addr64_t)slot[s].pages[i], slot[s].page_size, TRUE);
	}
	uint64_t t1 = mach_absolute_time();

	struct a6_blit b = {
		.src_iova = SLOT_IOVA(s) + offset, .src_w = w, .src_h = h, .src_pitch = pitch,
		.sw = w, .sh = h, .dst_iova = A6_GVA_FB, .dst_pitch = fb_pitch, .filter = true,
	};
	/* Fit the frame into the panel, keeping the aspect ratio. */
	if ((uint64_t)w * PANEL_H <= (uint64_t)PANEL_W * h) {
		b.dh = PANEL_H;
		b.dw = (uint32_t)(((uint64_t)w * PANEL_H + h / 2) / h);
	} else {
		b.dw = PANEL_W;
		b.dh = (uint32_t)(((uint64_t)h * PANEL_W + w / 2) / w);
	}
	b.dx = (PANEL_W - b.dw) / 2;
	b.dy = (PANEL_H - b.dh) / 2;

	static uint32_t dw[A6_BLIT_MAX_DW];
	lck_mtx_lock(&sweet_a6xx_lock);
	uint32_t n = a6_blit_ib(&gpu, &b, dw);
	a6_write(a6_pool_pa(&gpu, IB_OFF), dw, 4ULL * n);
	int rc = a6_submit_ib(&gpu, a6_pool_va(IB_OFF), n, 100);
	if (rc) {
		uint64_t far = 0;
		uint32_t syn = 0, fsr = a6_fault_status(&gpu, &far, &syn);
		a6_log("present: GPU timeout (fence %u, memory %u, smmu fsr %x far %llx); CPU copy stays\n",
		    gpu.fence, a6_fence_read(&gpu), fsr, far);
		(void)a6_clear_fault(&gpu);
		gpu_ready = false;	/* stop trying, do not wedge the display */
	}
	lck_mtx_unlock(&sweet_a6xx_lock);

	uint64_t t2 = mach_absolute_time();
	stat_clean_abs += t1 - t0;
	stat_gpu_abs += t2 - t1;
	if ((++stat_frames % 300) == 0) {
		uint64_t c = 0, g = 0;
		absolutetime_to_nanoseconds(stat_clean_abs, &c);
		absolutetime_to_nanoseconds(stat_gpu_abs, &g);
		a6_log("present: %llu frames, avg clean %llu us, GPU blit %llu us\n", stat_frames,
		    c / 1000 / 300, g / 1000 / 300);
		stat_clean_abs = stat_gpu_abs = 0;
	}
	if (!rc) {
		sweet_display_fit_x = (int)b.dx;
		sweet_display_fit_w = (int)b.dw;
	}
	if (out_dx) {
		*out_dx = b.dx;
	}
	if (out_dw) {
		*out_dw = b.dw;
	}
	return rc;
}

/* The display engine reads the panel with this row pitch; tell the 2D engine. */
void
sweet_gpu_panel_stride(uint32_t stride)
{
	/* SSPP_VIG0/1 YSTRIDE0 on the SDE, then flush CTL0. */
	a6_wr(0x0ae00000ULL + 0x5000 + 0x24, stride);
	a6_wr(0x0ae00000ULL + 0x5000 + 0x2000 + 0x24, stride);
	a6_wr(0x0ae00000ULL + 0x2000 + 0x18, 0x3);
	a6_log("panel stride %u\n", stride);
}

/* ======================================================================
 * GPU buffers and submission: what the Metal user client
 * (iokit/Kernel/arm/SweetAccelerator.cpp, selectors 1..7) needs behind it.
 *
 * The contract is the flame/rmx1931 one, so their phone/metal driver works
 * unmodified:
 *   0 params      -> gpu_id, chip_id, ready
 *   1 alloc(size) -> handle, gpu_va, size     (+ clientMemoryForType mmap)
 *   2 free(handle)
 *   3 import(surface id) -> handle, gpu_va, size
 *   4 submit(va, dwords, timeout) -> fence
 *   5 cache_flush(handle, off, len)
 *   6 submit_async(va, dwords) -> fence
 *   7 wait_fence(fence, timeout)
 * ====================================================================== */

#define A6_MAX_BO	256
#define BO_VA_BASE	0x1000000000ULL	/* 64 GB: above the surface slots */

struct a6_bo {
	uint64_t gpu_va, size;
	vm_offset_t kva;
	uint32_t npages;
	bool used;
};

static struct a6_bo bos[A6_MAX_BO];
static uint64_t bo_next_va = BO_VA_BASE;

int
sweet_a6xx_bo_alloc(uint64_t size, uint64_t *out_handle, uint64_t *out_gpu_va, uint64_t *out_size)
{
	if (!gpu_ready || size == 0) {
		return -1;
	}
	size = (size + (uint64_t)PAGE_MASK) & ~(uint64_t)PAGE_MASK;
	int h = -1;
	for (int i = 0; i < A6_MAX_BO; i++) {
		if (!bos[i].used) {
			h = i;
			break;
		}
	}
	if (h < 0) {
		return -1;
	}
	vm_offset_t kva = 0;
	if (kmem_alloc_contig(kernel_map, &kva, (vm_size_t)size, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return -1;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)kva);
	if (pn == 0) {
		kmem_free(kernel_map, kva, (vm_size_t)size);
		return -1;
	}
	uint64_t va = bo_next_va;
	bo_next_va += (size + 0xfffffULL) & ~0xfffffULL;

	lck_mtx_lock(&sweet_a6xx_lock);
	int rc = a6_map(&gpu, va, ptoa_64(pn), size);
	if (!rc) {
		a6_tlb_flush(&gpu);
	}
	lck_mtx_unlock(&sweet_a6xx_lock);
	if (rc) {
		kmem_free(kernel_map, kva, (vm_size_t)size);
		return -1;
	}
	bos[h].gpu_va = va;
	bos[h].size = size;
	bos[h].kva = kva;
	bos[h].npages = (uint32_t)(size >> PAGE_SHIFT);
	bos[h].used = true;
	*out_handle = (uint64_t)h;
	*out_gpu_va = va;
	*out_size = size;
	return 0;
}

int
sweet_a6xx_bo_free(uint64_t handle)
{
	if (handle >= A6_MAX_BO || !bos[handle].used) {
		return -1;
	}
	struct a6_bo *b = &bos[handle];
	/* No per-range unmap in the core: keep the GPU mapping until the arena is
	 * reset. 39-bit VA space and 256 buffers make the leak irrelevant; the
	 * IOSurface path (C++ side) uses the slot table, not this one. */
	kmem_free(kernel_map, b->kva, (vm_size_t)b->size);
	memset(b, 0, sizeof(*b));
	return 0;
}

int
sweet_a6xx_bo_info(uint64_t handle, vm_offset_t *kva, uint64_t *size)
{
	if (handle >= A6_MAX_BO || !bos[handle].used) {
		return -1;
	}
	*kva = bos[handle].kva;
	*size = bos[handle].size;
	return 0;
}

/*
 * Called from the C++ user client (iokit/Kernel/arm/SweetAccelClient.cpp) once
 * it has walked an IOSurface's physical pages: map them into GPU virtual
 * addresses and hand back the GPU address the client can submit against.
 */
int
sweet_a6xx_surface_import_pages(uint32_t surface_id, const uint64_t *pages, uint32_t npages,
    uint64_t *out_gpu_va, uint64_t *out_size)
{
	int slot = sweet_gpu_map_surface((uint64_t)surface_id, pages, npages, 4096);
	if (slot < 0) {
		return -1;
	}
	*out_gpu_va = SLOT_IOVA(slot);
	*out_size = (uint64_t)npages * 4096;
	return 0;
}

int
sweet_a6xx_submit(uint64_t ib_gpu_va, uint32_t dwords, uint32_t timeout_ms, uint32_t *out_fence)
{
	if (!gpu_ready || dwords == 0) {
		return -1;
	}
	lck_mtx_lock(&sweet_a6xx_lock);
	int rc = a6_submit_ib(&gpu, ib_gpu_va, dwords, timeout_ms);
	if (out_fence) {
		*out_fence = gpu.fence;
	}
	if (rc) {
		uint64_t far = 0;
		uint32_t syn = 0, fsr = a6_fault_status(&gpu, &far, &syn);
		a6_log("submit: timeout (fence %u, memory %u, smmu fsr %x far %llx)\n",
		    gpu.fence, a6_fence_read(&gpu), fsr, far);
		(void)a6_clear_fault(&gpu);
	}
	lck_mtx_unlock(&sweet_a6xx_lock);
	return rc;
}

int
sweet_a6xx_wait_fence(uint32_t fence, uint32_t timeout_ms)
{
	uint32_t waited = 0;
	while (waited <= timeout_ms) {
		if (a6_fence_read(&gpu) >= fence) {
			return 0;
		}
		a6_delay_ms(1);
		waited++;
	}
	return -1;
}

int
sweet_a6xx_bo_cache_flush(uint64_t handle, uint64_t offset, uint64_t len)
{
	if (handle >= A6_MAX_BO || !bos[handle].used) {
		return -1;
	}
	struct a6_bo *b = &bos[handle];
	if (offset >= b->size) {
		return -1;
	}
	if (len > b->size - offset) {
		len = b->size - offset;
	}
	flush_dcache64((addr64_t)(b->kva + offset), (unsigned)len, TRUE);
	return 0;
}

#endif /* ARM64_BOARD_CONFIG_GUACAMOLE */
