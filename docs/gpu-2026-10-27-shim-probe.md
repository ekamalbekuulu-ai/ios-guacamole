# 2026-10-27 GPU bring-up: guacamole a640 probe (session record)

Written by the agent that took over after `ios7pro-GPU-交接包.zip`. Read this together with `GPU-ADRENO640.md`, `NOTES.md` and the K90/redwood reference in `ref/`.

## 1. Why the Xiaomi 15 (MI15) GPU package is not the driver

`MI15_GPU_AllInOne_20261026` is Xiaomi 15 / SM8750 / Adreno 830v2 / `chipid 0x44050001` / gen8. This board is SM8150 / Adreno 640 / `chipid 0x06040001` / a6xx. Its own `STATUS.json` says `ios_gpu_acceleration_verified: false`, and its README says it is "not a working GPU driver". Nothing at register/firmware level transfers. What is worth taking is its methodology, not its code.

## 2. The reference that actually applies

| Reference | SoC / GPU | What it proves |
|---|---|---|
| `ref/ref-ios-rmx1931-SM8150-A640` (ios-flame) | SM8150 / **a640** | `xnu/patches/0095-flame-gpu-present.patch` has a working a640 kernel driver (`bsd/dev/sweet_a6xx.c`, ~1150 lines, environment-agnostic via `a6_*` hooks) plus Metal userland |
| `ref/k90-annibale` (Redmi K90) | SM8750 / a830 | the *method*: stepped ladder, `eagl.c + OSMesa + freedreno` userland, msm-DRM-as-IOKit interface |

Both were already downloaded by earlier sessions. Neither was used properly.

## 3. The a640 core maps onto this board almost unchanged

Every constant in `sweet_a6xx.h/.c` was checked against the stock DTB (`dtb/boot_unpacked/dtb`):

| Item | guacamole | core |
|---|---|---|
| kgsl-3d0 | `0x2c00000+0x40000` | `A6_GPU_BASE 0x02c00000` ✅ |
| gmu / pdc cfg / pdc seq | `0x2c6a000+0x30000` / `0xb290000` / `0xb490000` | same ✅ |
| gpucc | `0x2c90000+0x9000` | `A6_GPUCC` ✅ |
| kgsl-smmu (qsmmu-v500) | `0x02ca0000`, TBU `0x2cc5000/0x2cc9000` | `A6_SMMU 0x02ca0000` ✅ |
| cont_splash (fb) | `0x9c000000+0x2400000`, 1440x3120, stride 5760 | header had flame's `0x9c400000` ❗ |
| pil_gpu_region (zap) | **runtime value, see 4** | header hard-codes rmx1931's |

So this is not a driver port; it is two constants plus environment hooks.

## 4. `pil_gpu_region` must be read from the live device tree

- static DTB inside our boot.img: `pil_gpu_region = 0x98715000 + 0x2000`
- the tree ABL actually hands the device at boot (`backup/guacamole-live.dtb`): `0x98b15000 + 0x2000`

Every other `pil_*` carve-out moves by the same `+0x400000` (`pil_adsp_region` `0x1a00000 → 0x1e00000`), i.e. ABL rebases the group. Hard-coding either value is a coin flip — this is very likely why earlier rounds saw "wrong address" crashes and why the RSCC/PDC guesses never settled. `shim-rmx/fdt.c` now has `fdt_node_reg64()` and `gpuprobe_set_dtb()` reads the value at run time (logged as `zap: region ... (live DTB)`).

## 5. What was changed in `shim-rmx/` (commits `e34891f`, `96b2c35`, `eddd54 2`, `56c9cfd`)

| Change | Why |
|---|---|
| `ZAP_REGION` read from live DTB (`gpuprobe_set_dtb`) | see 4 |
| `FB_PA 0x9c000000`, `FB_STRIDE 5760`, `FB_W 1440`, band at `y=2500` | guacamole panel; `0x9c400000/4320` is flame's |
| GPU page-table mapping 5 → 9 × 2 MB | the band at y=2500 needs 18 MB of the fb mapped |
| `console.h` `FB_SPLASH_BASE 0x9c000000`, `FB_W/H 1440x3120` | `con_init()` range check rejected the real framebuffer, so the shim was always blind |
| no `ufs_init()` before the watchdog in probe builds | reading the gated UFS host hangs the bus (that is why `shim/boards/guacamole.h` has `PROBE_UFS 0`); it used to happen before `wdt_arm()`, i.e. black screen and no log forever |
| pstore writes behind `#ifdef SHIM_PSTORE` | `0xb7fc0000` is RMX1931's ramoops, not ours |
| log drawn on the splash fb after every stage (`probe_draw`), rows 0..70 | only observability channel with UFS off |
| watchdog disarmed after a successful probe | the log then stays on screen for a photo |
| `shim-rmx.yml`: keep `Image.gpuprobe` outside `out/` | `rm -rf out` in the next step deleted it |

## 6. Result of the first run (2026-10-27, GM1910 `108f0052`)

`fastboot boot work/probe-guacamole-v3.img` (3.0 MB, empty ramdisk, kernel = `Image.gpuprobe`, SHA-256 `A26E51783BAE…`) was accepted (`Sending/Booting OKAY`). 45 s later: **no `05C6:9008` (not EDL) and no fastboot enumeration** — the "probe finished and parked with the log on screen" signature. Screen contents not yet read.

## 7. Next

1. Read the probe log off the screen; the `zap: region … (live DTB)` line settles §4 for real.
2. Then move the same a640 core into the kernel (one new file + the `a6_*` hooks) behind `sweetgpu=N` stages, logging into the existing kernel ring so the log is pulled with `pull-log.ps1` instead of photographed.
3. Adopt the K90 lesson before any SMMU work: **every `gfx3d_*` SID in the device tree needs an SMR/S2CR route**. Their whole-GPU silent reset for 15+ rounds was one un-routed SID (LPAC, SID 1) leaving `THREAD_SYNC` bit 2 set, with `GFSR` reading 0 (false negative).
4. Userland, once the kernel submits: follow K90 (`eagl.c → OSMesa → gallium freedreno fd6 → fake libdrm → IOKit service whose selectors are Linux msm_drm ioctl numbers`), not rmx1931's Metal/Turnip path. fd6/a6xx is the *baseline* of that code base (it was written for a660), whereas Metal needs iOS-private plumbing and CA has no Vulkan renderer.
