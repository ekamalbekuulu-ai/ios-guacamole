# 把 K90(annibale) / HackiOS 的 GPU 栈移植到一加 7 Pro(guacamole / SM8150 / Adreno 640)

> 结论先说：**用户态那套几乎可以整块搬，内核那套只能当架构模板，寄存器/时序必须按 a640 重写。**
> 但这是目前最有希望的一条路 —— 因为对方已经在一台真机上把「XNU 内核 GPU 驱动 + Mesa freedreno + EAGL」
> 这条路走通了（K90：CoreAnimation 经 `eagl.c → OSMesa → freedreno` 在 Adreno 830 上画桌面）。
> 我们之前卡住的 RSCC，本质是**顺序问题**，而对方的代码里有一套完整的「分步 + 每步探针」方法论。

## 1. 移植源

| 来源 | 内容 |
|---|---|
| `iola1999/hackios-xnu-kernel`（分支 `k90-port`，上游 `oosexgang/hackios-xnu-kernel`） | XNU 侧 Adreno 8xx 驱动：`xnu/files/bsd/dev/adreno8*.{c,h}`（~350 KB，按子系统拆分）、`redwood_adreno.c`(154 KB) + `RedwoodGPU.cpp`（IOKit GPU 服务） |
| `iola1999/hackios-xnu-userland` | 用户态图形栈：Mesa freedreno 的 Darwin 补丁、`osmesa-es-profile`、`ir3-*` 修正、`tools/mesa/air/`（AIR→NIR，Metal 着色器前端）、`redwood-drm` |
| `MI15_GPU_Kit`（用户提供的 zip） | **不要作为移植源**：它是 Adreno 830v2 专用，且 `ios_gpu_acceleration_verified=false`（只做到独立绘图） |

本仓库里已 vendor 到 `port/k90-gpu/{kernel,userland,tools}`（见该目录的 commit）。

## 2. 可以直接用（几乎不用改）

- **Mesa freedreno 的 Darwin 补丁系列**：`freedreno-darwin.patch`、`osmesa-es-profile.patch`、`ir3-*`。
  freedreno 在 Linux 侧对 a640 的支持早已成熟，这些补丁解决的是「在 Darwin 上跑」的问题，与 GPU 代际无关。
- **EAGL shim 思路**：让 CoreAnimation 走 GL ES 渲染器，绕过未完成的 Metal 路径（`eagl.c`）。
- **`tools/mesa/air/`（AIR→NIR）**：以后要支持 Metal 着色器时的入口，先留着。
- **`redwood-drm`**：用户态到内核 GPU 服务的 DRM 风格接口。
- **分步 + 探针方法论**：对方每个阶段都有 `adreno8_step_N` / `announce` / `post_mortem`，
  并且日志由 loader 从内存读出来（README：「loader 支持日志与截图读取」）——**这正是我们最缺的东西**，
  我们的 shim 就是等价物。

## 3. 必须为 a640 重写（有权威依据）

我们已经在 `handoff/10-references-vendor/` 拿到了 `OnePlusOSS/android_kernel_oneplus_sm8150` 的
`adreno_a6xx_gmu.c` + `sm8150-gpu.dtsi` + `a6xx_reg.h`，所以这些不需要猜：

| 项 | K90 (A830 / SM8750) | 一加 7 Pro (A640 / SM8150) |
|---|---|---|
| GMU 窗口 | `0x3d37000`，size `0x68000`，字节偏移 = `4n - 0x37000` | `0x02c6a000`，size `0x30000`，字节偏移 = `4n - 0x6a000` |
| RSCC | **独立窗口**（`adreno8_rscc`） | **在 GPU 寄存器空间内**：RSCC 基址 `0x23400` dword → `RSCC_RSC_STATUS0_DRV0 = 0x02C8D010` |
| PDC cfg / seq | 各自窗口 | `0xB280000` / `0xB480000`（各 64 KiB） |
| RSC 握手 | `CONTROL_REQ/ACK` | 同（但仅睡眠唤醒需要；冷启动不需要） |
| GDSC / RPMh | 有 `gmxc.lvl` 等新资源 | 只有 `gfx.lvl` / `cx.lvl` / `mx.lvl`（我们已经能投票成功） |
| 固件 | `gen80000` 系列 | `a640_gmu.bin`（37,680 B，已提取，已能解析装载） |

## 4. 里程碑

1. **M1 — a640 的 GMU CM3 启动**（当前唯一硬卡点）
   - 现在的实测：`RSCC/PDC` **能读**、**一写就把 SoC 写进 EDL**；父时钟 `gpll0/gpll0_out_even` 打开后
     `memnoc_gfx` / `snoc_dvm` 仍是 halted。
   - 借用对方的顺序：**先确认 GPUCC 的 CX/GX GDSC 与 BCR 释放，再动 RSCC**；`adreno8_cx.c` 里
     `adreno8_gpucc_answers()` / `adreno8_cx_is_on()` 那种「先证明块活着再写」的写法要照抄。
   - 每写一个寄存器前先读回、失败立即停，绝不连写。
2. **M2 — HFI 握手 + 环形队列 + CP 启动**：以 `adreno8_hfi.c` / `adreno8_hwsched.c` 为模板，
   a640 的 HFI 队列布局我们已核对过（交接包 §7.3 已验证过 iova/type/size）。
3. **M3 — 第一帧 GPU 绘制 + 回读校验**：对方是 `gen8-a8xx-blit` + `pixel-proof`；
   我们对应做一次 2D 填充并把 4096 字节回读比对（这是「GPU 真的能跑」的硬证据）。
4. **M4 — Mesa freedreno + EAGL 接进 iOS 用户态**：用户态基本照搬，只需把 `fd_pipe` 的设备节点
   换成我们的 GPU 服务。

## 5. 必须先解决的工具问题（否则每一步都靠拍照）

对方靠 loader 读日志。我们对应的做法，按代价从低到高：

1. **让 shim 读 XNU 写在内存里的日志环并打到屏幕**（最简单，仍然要拍照，但一次能看完整一屏）；
2. **让 shim 把日志通过 USB 吐出来**（shim 跑在 ABL 之后，DWC3 还在 fastboot 状态，理论上可复用
   现成的 gadget 代码 —— 但我们在 XNU 里做这事已经证明会崩，放在 shim 里风险低得多）；
3. **9008 + firehose 读 userdata 里的日志环**（需要一加签名的 loader；用户电脑上有一加救砖包，
   但 `.ops` 只是外层容器，loader 不直接可得）。
