# Adreno 640 (SM8150) GPU 可行性报告

**日期**：2026-10-20（2026-10-21 更新）
**对象**：一加 7 Pro（`guacamole`，SM8150 / 骁龙 855，Adreno 640）上的 ios-guacamole 移植
**状态**：阶段 0.3 已在真机验证（XNU patch 0088：`gfx.lvl` ARC corner 1
投票后 GX GDSC `gdscr` BIT(31)=1、`cfg` BIT(16)=1；RBBM/CP 仍读回 0，
因为 GPU 核心/GMU 时钟未开）。当前系统仍完全依赖 CoreAnimation 软件渲染，
没有 GPU 驱动。
**参考**：[UniXNU/NativeAdreno](https://github.com/UniXNU/NativeAdreno)、
[UniXNU/NativeMetal](https://github.com/UniXNU/NativeMetal)、Mesa
freedreno / Turnip、[UniXNU](https://github.com/UniXNU) 组织下的配套模块。

---

## 0. 结论（TL;DR）

| 问题 | 回答 |
| --- | --- |
| 能不能驱动 Adreno 640？ | **能**，但属于"移植一个已存在的 XNU 驱动"，不是从零逆向 |
| 有没有现成参考？ | 有：UniXNU 已在 **XNU + Adreno 740（SM8550）** 上跑通 GPU 命令与 scanout；a640 的完整寄存器/电源序列在 Mesa `a6xx` 驱动里 |
| 我们离"GPU 点亮"有多远？ | 阶段 0（上电 + GMU + 一次 PM4 提交）约 **1~3 周**，风险集中在 zap 与 GMU HFI |
| 离"iOS UI 用 GPU 合成"有多远？ | 还要加用户态 Metal 前端（NativeMetal 路线）+ 着色器编译，合计 **3~6 个月** 量级（部分依赖上游进度） |
| 值不值得做？ | 现在 UI 已经能用。GPU 的收益是流畅度、功耗、Metal 应用；建议**先做阶段 0 验证**，再决定是否投入后续 |

**一句话**：可行性没有疑问，工作量很大；建议把它当成独立项目，从"GPU 能把面板刷成纯色"开始。

---

## 1. 参考项目拆解

### 1.1 NativeAdreno —— 内核态（最重要）

> "Native XNU Adreno 740 GPU driver. Owns GMU startup, the CP ring, BOs,
> native 4 KiB SMMU contexts, fences and cache maintenance. Linked into XNU
> by `unixnu build kernel`."

模块划分（来自仓库文件）：

| 文件组 | 职责 |
| --- | --- |
| `NativeAdrenoAccelerator/Client/Service/UserClient` | IOKit 服务与用户客户端；版本化 wire ABI |
| `NativeAdrenoPwrup*` | **GPU 上电寄存器表**（a740 专用，`buildA740()`） |
| `NativeAdrenoA740HwInit.h` | a740 硬件初始化常量 |
| `NativeAdrenoFirmware*` / `*FirmwareDefaults` | 固件校验（长度 + SHA-256）与视图 |
| `NativeAdrenoGmu*`（Config/Context/Memory/Platform/Service） | GMU 冷启动、HFI、内存布局 |
| `NativeAdrenoCpInit` | CP（命令处理器）初始化 + ring |
| `NativeAdrenoRing` / `Submit` / `Irq` / `Timer` | 提交环、fence、中断、时间戳 |
| `NativeAdrenoSmmu*` / `Vm` / `Memory` | SMMU 上下文、IOVA、BO 映射 |
| `NativeAdrenoBoard` | 解析 DT 的 `reg` / `iommus`（stream ID） |
| `NativeAdrenoCpu` | 缓存维护 |
| `NativeAdrenoPublish` | 向 IORegistry 发布设备 |

对本项目的意义：**内核驱动骨架可以直接照搬**，a640 只需要替换数据表与序列。

### 1.2 NativeMetal —— 用户态

> "Metal API frontend. Publishes a Metal device through `MTLAddDevice`,
> translates AIR with metal2vulkan and submits through Turnip and NativeAdreno."

- 用私有 API **`MTLAddDevice`** 向系统注册一个非 Apple 的 Metal 设备；
- 把 Metal 着色器（AIR，Apple Intermediate Representation）翻译成 Vulkan（SPIR-V）或 IR3；
- 通过 Turnip / NativeAdreno 提交。

**已在本机验证**：iOS 16.7.16 的 `Metal` 框架里 **确实存在 `MTLAddDevice`**：

```
0x181b6e20c: "MTLAddDevice"          image=Metal
0x181a1b368: _MTLAddDevice           (__TEXT.__text, Metal)
0x0:         ___MTLAddDevice_block_invoke   (symbols 子缓存)
```

这是整条用户态路线的**前提条件**，现在确认成立 ✓。

### 1.3 freedreno / Turnip —— 权威寄存器与编译参考

- **freedreno**（内核侧）：`a6xx_gpu.c`、`a6xx_gmu.c`、`a6xx_hfi.c`、`a6xx.xml.h`
  —— a640 的上电序列、GMU 协议、寄存器偏移、SMMU SID 全在里面；
- **Turnip**（Vulkan 用户态）：a6xx 的 Vulkan 驱动，含 **IR3** 着色器后端；
- **AirNir**（UniXNU）：AIR → NIR → IR3 的编译器套件，绕开 Vulkan 直接生成 Adreno 机器码。

### 1.4 其他配套（UniXNU）

| 模块 | 用途 | 我们是否需要 |
| --- | --- | --- |
| `ArmSmmu4K` | SMMUv2 native 4 KiB 页表工具 | **需要**（SM8150 的 apps SMMU 是 SMMUv2） |
| `AdrenoPM4` | PM4 包编码 | 需要（提交命令） |
| `AdrenoFirmware` | GMU 固件 + HFI kit | 需要（a640 数据要自己填） |
| `NativeMobileDisplay` | 移动显示与 GPU scanout 交接 | 可参考（我们已有自己的 present 路径） |
| `NativeUfs` / `NativeHidInput` / `ArmCpu` | 存储/输入/CPU | 我们已有等价实现 |

许可证：NativeAdreno / NativeMetal / AirNir / AdrenoPM4 / ArmSmmu4K 均为 **MIT** ✓。

---

## 2. 我们当前的图形栈（基线）

```
iOS App / SpringBoard
      │
      ▼
CoreAnimation（CA::OGL::SW，纯 CPU 光栅化）
      │  IOSurface（内核 IOCoreSurfaceRoot）
      ▼
IOMobileFramebuffer（AppleCLCD，我们实现）
      │  30 Hz 定时拷贝，letterbox 1125x2436 → 1440x3120
      ▼
ABL 留下的 splash framebuffer（0x9c000000，36 MiB）
      │
      ▼
面板（S6E3HC2，1440x3120）
```

已经具备、对 GPU 项目有直接价值的组件：

- IOSurface / IOMobileFramebuffer 语义（GPU 渲染目标可以直接复用）；
- RPMh（电源轨投票）、GIC（中断）、UFS（读固件）、TLMM；
- 稳定的 present 路径（GPU scanout 只要写同一块内存）。

---

## 3. 目标架构

```
Metal.framework（iOS 16 原生）
      │  MTLAddDevice 注册的插件设备
      ▼
NativeMetal 前端（Metal API → AIR 翻译）
      │  AIR → NIR → IR3（AirNir）  或  AIR → SPIR-V（Turnip）
      ▼
用户态提交 → NativeAdreno 用户客户端（BO / Submit / Fence）
      ▼
NativeAdreno 内核驱动（GMU、CP ring、SMMU、BO）
      ▼
Adreno 640  ──(scanout)──►  面板 framebuffer
```

---

## 4. 本机硬件实证（来自手机自身 DTB）

| 项 | 值 | 说明 |
| --- | --- | --- |
| GPU 节点 | `/soc/qcom,kgsl-3d0@2C00000`，`compatible = "qcom,kgsl-3d0"`，`status = "ok"` | 下游 KGSL 驱动节点 |
| **chipid** | **`0x6040001`** | 6 = a6xx，4 = a640，rev `0x40001` |
| 寄存器窗口 | `0x2c00000` + `0x40000`；`0x2c61000` + `0x800`；**`0x6900000` + `0x44000`**；`0x780000` + `0x6fff` | 下游 `reg` 列表（RBBM/GMU/core/wrapper 混合，需按 reg-names 对应） |
| 中断 | GIC SPI **300**（`interrupts = <0 0x12c 0>`） | GPU 中断 |
| 时钟 | `rbbmtimer_clk` 等（`clocks`/`clock-names`） | 由 GPUCC 提供 |
| 电源 | `vddcx-supply`、`vdd-supply`；`regulator-names = "vddcx"` | CX 与 GFX 电源 |
| Quirks | `qcom,gpu-quirk-secvid-set-once`、`qcom,gpu-quirk-cx-gdsc` | **CX GDSC 顺序需要注意** |
| UBWC | `qcom,ubwc-mode = 3` | 压缩格式，初期可关闭 |
| 最高 bank | `qcom,highest-bank-bit = 0xf` | 36 位物理地址 |
| GPUCC | `/soc/qcom,gpucc`：`0x2c90000` + `0x9000`，`compatible = "qcom,gpucc-sm8150"` | 时钟/复位；`vdd_cx`、`vdd_mx` |
| SMMU | `/soc/apps-smmu@0x15000000`：`0x15000000` + `0x100000`，`compatible = "qcom,qsmmuv500"`，`#iommu-cells = 2`，`qcom,use-3-lvl-tables` | **SMMUv2（SMMU-500）** ✓ |
| zap | `/soc/qcom,kgsl-hyp`：`qcom,pas-id = 0xd`，`qcom,firmware-name = "a640_zap"` | 安全世界认证路径 |
| 固件 | `a640_gmu.bin`、`a630_sqe.fw`、`a640_zap.mbn` | 见 linux-firmware / 手机 vendor 分区 |

> 结论：**SM8150 的 GPU SMMU 是 SMMUv2**，与 UniXNU 的 `ArmSmmu4K` 完全对口；
> 这是整个移植里少有的"直接复用"项。

---

## 5. 需要新增的内核模块

| # | 模块 | 职责 | 参考 | 难度 |
| --- | --- | --- | --- | --- |
| 1 | **GPUCC 时钟/复位** | 打开 gmu/rbbmtimer/gfx3d 时钟，解除复位 | mainline `gpucc-sm8150.c` | 中 |
| 2 | **电源域** | CX/GX GDSC、`vddcx`/`vdd` 电压（RPMh） | msm `a6xx_gmu` + `gdsc` | 中 |
| 3 | **SCM / zap** | `qcom_scm_pas_*`：加载并认证 `a640_zap.mbn`，让 GPU 退出安全态 | msm `a6xx_gpu.c` + 我们已有 SMC 通路 | **高**（失败只回错误码） |
| 4 | **SMMUv2 驱动** | 上下文、SID、DOMAIN_ATTR、故障中断 | `ArmSmmu4K` | 中 |
| 5 | **固件加载** | 从 vendor 分区读 `a640_gmu.bin` / `a630_sqe.fw` | 我们的 UFS 驱动 + GPT | 低 |
| 6 | **GMU** | 冷启动、HFI 消息、电源状态 | msm `a6xx_gmu.c` / `a6xx_hfi.c` | **高**（a640 协议版本与 a740 不同） |
| 7 | **CP / ring** | CP 初始化、SQE 加载、ringbuffer、fence | `NativeAdrenoCpInit` + msm | 中 |
| 8 | **BO / IOVA** | 分配、映射、缓存维护、导入 IOSurface | `NativeAdrenoVm/Memory` | 中 |
| 9 | **用户客户端 / ABI** | 暴露 BO/提交/fence 给用户态 | `NativeAdrenoABI.h` | 低 |
| 10 | **中断/时间戳** | GPU IRQ、fence 信号、timestamp | `NativeAdrenoIrq/Timer` | 低 |
| 11 | **scanout 交接** | GPU 渲染目标 → 面板内存 | `NativeMobileDisplay` + 我们的 present | 低 |

---

## 6. a740 → a640 移植差异

| 项 | a740（sheng） | a640（本项目） | 影响 |
| --- | --- | --- | --- |
| GPU 代际 | a7xx | **a6xx**（同族，PM4 大体兼容） | 中 |
| GMU 固件 | `a740_sqe.fw` / 新版 GMU | **`a640_gmu.bin` + `a630_sqe.fw`** | 高（HFI 版本） |
| 上电寄存器表 | `NativeAdrenoPwrupRegs.h`（a740 常量） | 换成 a640 的 `pwrup_reglist` | 中（纯数据） |
| 时钟 | SM8550 GPUCC | **SM8150 GPUCC（`qcom,gpucc-sm8150`）** | 中 |
| 电源域 | 新 GDSC 布局 | **CX/GX GDSC + `qcom,gpu-quirk-cx-gdsc`** | 中 |
| SMMU | 自带上下文实现 | **apps SMMU（SMMUv2），SID 来自 DT** | 中 |
| zap | `a740_zap.mbn` | **`a640_zap.mbn`，PAS id 0xd** | 高（安全世界） |
| 固件校验 | SHA-256 白名单 | 需为 a640 三个 blob 生成规则 | 低 |
| 显示 | sheng 自有路径 | 我们已有 present/letterbox | 低 |

**预计复用率**：结构与 IOKit/提交/BO/fence 层 ~80%，a640 专属部分 ~20%（但正好是最容易踩坑的 20%）。

---

## 7. 风险登记表

| # | 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- | --- |
| R1 | **zap / 安全世界**：TZ 拒绝认证，GPU 无法退出安全态 | 中 | 致命（阻断） | 先从手机 vendor 固件取**签名过的** `a640_zap.mbn`；严格照 msm 的 PAS 流程；保留错误码日志 |
| R2 | **GMU HFI 版本**：a640 协议与 a740 不同，消息格式对不上 | 中高 | 高 | 直接以 msm `a6xx_gmu/hfi` 为准，逐条对照；先做"GMU 启动成功"这一最小验证 |
| R3 | SMMU 上下文/SID 配置错误，GPU 访问越界 | 中 | 高 | 初期关闭 UBWC、用 identity/单上下文；开 SMMU 故障中断并打印 |
| R4 | GPUCC 时钟/GDSC 顺序错误，挂死总线 | 中 | 高（可能复位） | 照 mainline `gpucc-sm8150` + `gdsc` 顺序；逐步加 print |
| R5 | 固件取不到（vendor 分区读取） | 低 | 中 | 从官方 ROM 提取后放到 iOS 数据卷（不分发） |
| R6 | **用户态 Metal 不认插件设备** | 低（`MTLAddDevice` 已确认存在 ✓） | 高 | 先用 NativeMetal 的最小例子验证注册；不行就只做 Vulkan/自定义合成 |
| R7 | **着色器运行时编译太慢**（iOS 的 UI 预编译包是 AGX 的） | 高 | 中（体验） | 预编译常用 UI 包；接受首次卡顿；或只对特定 App 开 GPU |
| R8 | 工作量大、长期无收益 | 高 | 中 | 分阶段，每阶段有独立验收；阶段 0 失败就停 |
| R9 | **GX/GMU 上电缺少 AOP QMP/ARC 契约**：vendor `gdsc-regulator.c` 在清 CX `SW_COLLAPSE` 前发送 `{class: clock, res: gpu_noc_wa}`；GX 还依赖 `gfx.lvl` ARC 电压 | 中 | 高 | CX 已验证无需 QMP；GX/GMU 前先补 ARC 表检查，若 GX power-up 超时或总线异常再补 AOP mailbox |

---

## 8. 分阶段计划

### 阶段 0：GPU 上电 + GMU + 一次 PM4 提交（建议先做）

**目标**：不接任何用户态，内核里直接把面板内存刷成纯色/测试图案。

步骤：
1. GPUCC 时钟 + GDSC + RPMh 电压；
2. SCM 加载/认证 zap；
3. 从 DT/efuse 确认 GPU `chipid` 为 `0x6040001`（不是 GPU 寄存器）；
4. 加载 `a640_gmu.bin`，启动 GMU，跑通 HFI 握手；
5. 加载 `a630_sqe.fw`，初始化 CP ring；
6. 提交一个 PM4 `EVENT_WRITE`/`CLEAR` 类命令写到 `0x9c000000`；
7. 屏幕上出现图案即成功。

**当前进度（2026-10-21，真机验证）**：patch 0085 完成了步骤 1 的前半段——
GCC GPU 时钟、CX GDSC 上电、GPU RBBM 读回（`sweetgpu=2`）。实测
`cx after ... hw=0x80001fe0`，BIT(31) 置位，且系统继续启动到 SpringBoard；
RBBM/CP 读回全 0 是因为 GX 仍关闭，不是寄存器总线故障。
patch 0086 的 `sweetgpu=3` 尝试用 vendor GX SW-reset/AON reset/clamp 序列
单独给 GX 上电：`cfg=0x1080400`，PWR_UP 始终不置位，说明缺 `gfx.lvl`
ARC/regulator vote；失败路径把 GX 重新 collapse，系统仍启动到桌面。
patch 0087 读出 command-DB 三张表：`gfx.lvl` `0x30050`（0/16/48/64/128/
192/224/256/320/384/416）、`cx.lvl` `0x30000`（0/16/64/128/192/256/384/416）、
`mx.lvl` `0x30010`（0/16/128/192/256/320/384/416）。
patch 0088 的 `sweetgpu=4` 在 GX 序列前向 `gfx.lvl` 写 RPMh corner 1；
CPU 侧 ARC 投票数据是 **corner index**（`rpmhpd.c`: `cmd.data = corner`），
不是 GMU HFI 的 `(level<<16)|(sindex<<8)|pindex`。实测
`gx after gdscr=0xf8222000 cfg=0x70400`（PWR_ON/PWR_UP 均置位），随后
collapse 并回投 corner 0；iOS 仍启动到 SpringBoard。GX 上电本身已解决；
RBBM/CP 仍为 0，下一步是 GX GPU/GMU 时钟、`a640_gmu.bin`、zap、SMMU、CP。
CX 上电在本机没有发送 AOP `gpu_noc_wa` QMP 也成功了，但 GX/GMU 阶段的
QMP 仍需验证（见 R9）。

**验收**：面板出现非 ABL splash 的纯色块；GPU 中断计数增加；再次启动不破坏现有 GUI。
**工期**：1~3 周（有参考代码、无硬件故障的前提下）。
**风险**：R1、R2、R4、R9。

### 阶段 1：用户客户端与 BO

- 实现 `NativeAdrenoABI` 的 Query / BoAlloc / BoMap / BoCpuMap / Submit / WaitFence / GetTimestamp；
- 用户态测试程序（iOS 上的一个静态二进制，或先在内核里自测）做一次三角形/清屏；
- **验收**：用户态分配 BO、提交、fence 正确返回。

**工期**：2~4 周。

### 阶段 2：Vulkan 或 IR3

- 路线 A：Turnip（Vulkan）接到我们的 ABI；
- 路线 B：AirNir（AIR→NIR→IR3）直接生成 a6xx 机器码；
- **验收**：跑通一个 Vulkan triangle / 一个 Metal 命令缓冲。

**工期**：4~8 周。

### 阶段 3：Metal 前端接入系统

- NativeMetal 的 `MTLAddDevice` 插件打包进 iOS 用户态；
- 让 backboardd/CoreAnimation 选择 Metal 渲染器；
- **验收**：主屏幕动画由 GPU 合成，CPU 占用明显下降。

**工期**：8~16 周（与上游进度强相关）。

---

## 9. 预检清单（低成本、先做）

| # | 检查 | 方法 | 结果 |
| --- | --- | --- | --- |
| P1 | iOS 16.7.16 是否支持 `MTLAddDevice` | `ipsw dyld symaddr` / `str` 查 dyld 缓存 | ✅ **存在**（`_MTLAddDevice` @ `0x181a1b368`，Metal） |
| P2 | GPU 是否被 ABL 断电 | 加一个内核探针读 `0x6900000`/GPUCC 状态 | ✅ 0084：GPUCC 可读；CX/GX GDSC 均为 off（`0x222001`，`cx_hw=0x5ff4`） |
| P3 | vendor 分区固件可取 | 从官方 ROM 解包 `a640_gmu.bin`/`a630_sqe.fw`/`a640_zap.mbn` | ✅ `research/firmware/sm8150/`（本地，gitignored） |
| P4 | GPU SID / iommus | 从实时 DTB 读 `kgsl-3d0` 的 `iommus`（解析器已就绪） | ✅ 实时 DT：SID 0/2，GMU SID 4/5；独立 GPU SMMU `0x2ca0000` |

---

## 10. 备选方案

| 方案 | 内容 | 优点 | 缺点 |
| --- | --- | --- | --- |
| A. 维持软件渲染 | 什么都不做 | 零风险；当前已可用 | 20~30 fps、CPU 占用高、Metal App 不可用 |
| B. 只做 GPU 清屏/拷贝 | 阶段 0 单独存在 | 验证硬件、可用于快速清屏/过渡动画 | 对 UI 帮助有限 |
| C. 完整 GPU 合成 | 阶段 0~3 | 流畅、省电、支持 Metal | 3~6 个月量级 |
| D. 等上游 | 等 UniXNU 把 a6xx 支持做完 | 省力 | 时间不可控 |

**建议**：先做 **阶段 0 + 预检 P2/P3/P4**；如果 GMU 能起来，再决定是否走 C。

---

## 11. 参考资料

- UniXNU/NativeAdreno — https://github.com/UniXNU/NativeAdreno
- UniXNU/NativeMetal — https://github.com/UniXNU/NativeMetal
- UniXNU（总项目） — https://github.com/UniXNU/UniXNU
- AirNir（AIR→NIR→IR3） — https://github.com/UniXNU/AirNir
- AdrenoPM4 / AdrenoFirmware / ArmSmmu4K / NativeMobileDisplay — 同组织
- Mesa freedreno/Turnip — https://gitlab.freedesktop.org/mesa/mesa
- linux-firmware qcom：`a640_gmu.bin`、`a630_sqe.fw`、`sm8150/a640_zap.mbn`
- mainline `gpucc-sm8150.c`、`sm8150.dtsi`（GPU/SMMU 节点）

---

## 附录 A：地址与固件速查

| 项 | 值 |
| --- | --- |
| GPU 控制块 | `0x2c00000`（+0x40000） |
| GPU 寄存器块 | `0x6900000`（+0x44000） |
| 其他窗口 | `0x2c61000`（+0x800）、`0x780000`（+0x6fff） |
| GPU IRQ | GIC SPI 300 |
| GPUCC | `0x2c90000`（+0x9000），`qcom,gpucc-sm8150` |
| apps SMMU | `0x15000000`（+0x100000），`qcom,qsmmuv500`，3 级页表，`#iommu-cells = 2` |
| zap | `/soc/qcom,kgsl-hyp`，PAS id `0xd`，`a640_zap` |
| chipid | `0x6040001`（a640） |
| 固件 | `a640_gmu.bin`、`a630_sqe.fw`、`a640_zap.mbn` |
| 面板内存 | `0x9c000000`（36 MiB，ABL splash） |

## 附录 B：NativeAdreno 用户态 ABI（可直接沿用）

`NativeAdrenoABI.h` 定义的方法：

| # | 方法 | 用途 |
| --- | --- | --- |
| 1 | Query | 查询版本/能力 |
| 2 | BoAlloc | 分配缓冲区对象 |
| 3 | BoMap | 映射到 GPU IOVA |
| 4 | BoFree | 释放 |
| 5 | Submit | 提交命令 |
| 6 | WaitFence | 等待完成 |
| 7 | GetTimestamp | GPU 时间戳 |
| 8 | GetStatus | 状态/错误 |
| 9/10 | Set/GetMetadata | 元数据 |
| 11/12 | Create/CloseQueue | 队列 |
| 13/14 | BoCpuMap/BoCpuUnmap | CPU 映射 |
| 15 | GetDeviceInfo | 设备信息 |
| 16 | BoImport | 导入外部缓冲区（如 IOSurface） |

ABI 是"固定大小记录 + 版本号"的设计，未知版本/方法/标志一律拒绝，适合直接移植。
