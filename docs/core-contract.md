# 核心与宿主契约

## 接口层次

| 接口 / 目标 | 用途 |
|---|---|
| `nes::inspect()` / `core_api/cartridge.hpp` | 无分配检查 ROM 字节流，提取头信息、Mapper、容量和时序 |
| `nes::Host` / `core_api/host.hpp` | 宿主服务、输入、扫描线、帧和日志回调 |
| `nes::infones_adapter::run()` / `nes_host` | 同步执行一次游戏，组合加载、模拟和卸载 |
| `load()`、`validate_board()`、`unload()` / `nes_cartridge` | 底层加载与生命周期控制，声明于 `adapters/infones/cartridge.hpp` |
| `InfoNES_System.h`、`InfoNES_Port.h` / `nes_core` | InfoNES 底层宿主钩子及分配接口 |

应用优先使用 `run()`。它是单实例、同步调用：加载成功后执行 `InfoNES_Cycle()`，退出时卸载。返回 `true` 表示正常结束，加载或运行期分配失败返回 `false`，通过可选的 `const char **error` 返回静态错误字符串，无须释放。

## 宿主回调

`nes::Host` 定义以下回调：

| 回调 | 契约 |
|---|---|
| `service()` | 每扫描线提供服务机会；不得依赖是否发布显示帧 |
| `input(pad1, pad2, quit)` | 填写 NES 两个端口的按键与退出请求；缺省按键为零，缺少此回调会请求退出 |
| `line(y, pixels, mask, pal)` | 同步消费 256 像素行、PPU 遮罩及 PAL 标志；像素缓冲在返回后可被复用 |
| `frame()` | 一帧完成，可发布画面和等待宿主帧期限；无返回值 |
| `message(text)` | 同步消费日志或错误字符串，不保留借用指针 |

按键 bit 0–7 依次为 **A、B、Select、Start、上、下、左、右**。退出使用 `input()` 的 `quit`，由适配层映射为 `PAD_SYS_QUIT`。`InfoNES_LoadFrame()` 的返回值不控制退出。

`Host` 和 ROM 均由调用方拥有，从进入 `run()` 到返回保持有效；ROM 地址稳定且内容不可修改。回调在模拟线程同步执行，不得重入 `run()`、`load()`、`unload()` 或并发访问核心。异步显示必须在回调内复制数据，再发布独立缓冲。

## 加载与资源生命周期

`inspect()` 检查文件签名、格式、容量和数据边界；`validate_board()` 进一步检查 Mapper、板型和 RAM 布局。仅格式检查通过不代表可以运行。

`load()` 先验证，再卸载已有卡带。格式或板型检查失败时保留已加载卡带；开始替换后的分配或初始化失败会清理资源。成功后核心借用 ROM 中的 PRG / CHR，复制头信息、清空 SRAM，并在存在 Trainer 时将其导入 SRAM。`unload()` 结束借用并释放核心资源，可重复调用。

这些接口和所有核心调用仅归模拟线程；显示核只消费已经发布的独立缓冲。不能混用正在运行的 `run()` 与手动加载、卸载流程。

`InfoNES_Port.h` 提供分配 / 释放、大内存能力、精灵限制选项和每扫描线服务。分配失败返回空指针，释放接受空指针。核心锁存运行期分配失败，并在扫描线安全边界退出；`run()` 检查失败状态、报告错误并卸载。

直接使用底层 `InfoNES_Reset()` 时，失败后的清理由调用方负责：调用可重复执行的 `InfoNES_Fin()`。ROM 释放和声音关闭钩子也须容忍重复调用。

## 卡带支持范围

公共加载入口接纳 Mapper **0、1、2、3、4、7、8、11、15、18、23、24、25、32、33、34、41、48、66、69、70、71、72、75、76、78、79、86、87、88、89、92、97、112、113、133、140、177、184、206、226、240、241、242、243、244、268**，并施加以下约束：

- PRG ROM 容量为二的幂，通常为 16–512 KiB；Mapper 15、34、177、226、241 上限为 1 MiB。存在 CHR ROM 时为 8–256 KiB，并受下列板型限制。
- CHR ROM 容量为二的幂，Mapper 4 例外：允许以 8 KiB 为单位追加银行，按实际 1 KiB 银行数取模寻址。
- Mapper 0（NROM）：PRG 不超过 32 KiB，CHR ROM 不超过 8 KiB。
- Mapper 3（CNROM）：PRG 不超过 32 KiB，必须有 CHR ROM 且不超过 32 KiB。
- Mapper 2（UxROM）与 Mapper 7（AxROM）：仅使用 CHR RAM。
- Mapper 2、3、7 的 NES 2.0 submapper 2 模拟总线冲突；其中 7.2 的 PRG 上限为 256 KiB。submapper 0 保留无冲突写入兼容行为。
- Mapper 8：仅接纳无 Trainer 的固定 GNROM 模式，PRG 不超过 128 KiB，必须有 CHR ROM 且不超过 32 KiB；不模拟完整 FFE 模式切换和 IRQ。
- Mapper 11（Color Dreams）：PRG、CHR ROM 均不超过 128 KiB，也接纳 8 KiB CHR RAM；旧编号覆盖多种板型，保留无冲突写入兼容行为。
- Mapper 15、241：仅接纳 8 KiB CHR RAM。Mapper 15 保留旧改版 ROM 所需的 CHR 可写和通用 SRAM 行为，不模拟原多合一卡的 CHR 写保护。
- Mapper 18（Jaleco SS8806）：PRG 不超过 256 KiB，必须有 CHR ROM；支持 RAM 使能/写保护及 4/8/12/16 位 IRQ 计数，按扫描线推进，不提供逐 CPU 周期精度。扩展语音芯片未模拟。
- Mapper 33、48、66 与 69（FME-7 / Sunsoft 5B）：必须有 CHR ROM。Mapper 33 按 TC0190 接线，无 IRQ；需要 TC0690 IRQ 的卡带应正确声明 Mapper 48。
- Mapper 23（VRC2/VRC4）、24（VRC6）与 25（VRC4）：PRG 不超过 256 KiB，必须有 CHR ROM；23 额外支持 NES 2.0 submapper 2（VRC4e）、3（VRC2b），25 额外支持 submapper 2（VRC4d）。VRC IRQ 沿用扫描线级实现，不提供逐 CPU 周期精度。23.3 无 IRQ；无 PRG RAM 时提供一位锁存，开放总线以地址高字节近似。
- Mapper 23.2 支持 WRAM 使能，容量不超过 4 KiB 时仅映射 `$6000–$6FFF`；未明确板型的 23.0 保留兼容 RAM 访问，不套用 VRC4 的使能规则。
- Mapper 32（Irem G-101）：仅 submapper 0，PRG 不超过 256 KiB，CHR ROM 不超过 256 KiB；支持即时 PRG 窗口交换和横/纵镜像，不接纳四屏与固定单屏变种。
- Mapper 34：旧头按 CHR 类型区分 BNROM（CHR RAM）与 NINA-001（CHR ROM）；NES 2.0 支持 submapper 1（NINA-001）、2（BNROM）。BNROM PRG 上限 1 MiB，并模拟总线冲突；NINA-001 PRG/CHR ROM 均不超过 64 KiB，寄存器位于 `$7FFD–$7FFF`，写入同时保存到 SRAM。两者不接纳四屏。
- Mapper 140（JF-11/14）：PRG、CHR ROM 均不超过 128 KiB；寄存器位于 `$6000–$7FFF`，不写入 SRAM。
- Mapper 226：PRG 上限 1 MiB，8 KiB CHR RAM；支持双寄存器、16/32 KiB 银行与横/纵镜像，不接纳四屏。
- Mapper 70：PRG、CHR ROM 均不超过 128 KiB，必须有 CHR ROM；沿用旧 70/152 文件的镜像控制兼容行为。
- Mapper 71：PRG 不超过 256 KiB，仅使用 CHR RAM。
- Mapper 177（Hengge）：32 KiB PRG 银行切换，上限 1 MiB，仅使用 8 KiB CHR RAM；支持横/纵镜像控制，不接纳四屏声明。NES 2.0 必须声明 8 KiB PRG RAM 或 NVRAM；不提供持久化存档。
- Mapper 206（DxROM）：PRG 不超过 128 KiB，必须有 CHR ROM 且不超过 64 KiB；额外接纳 submapper 1 的固定 32 KiB PRG，无 IRQ，使用文件头声明的镜像方式。
- Mapper 268（COOLBOY）：仅接纳 submapper 0、CHR ROM 板型；大容量多合一卡、扩展 CHR RAM 和其他 submapper 尚未开放。
- 离散逻辑及无 IRQ 板型的额外限制如下，容量仍须满足通用几何及 RAM 约束：

| Mapper | PRG 上限 | CHR 类型及上限 | 板型限制 |
|---|---:|---|---|
| 41、113 | 256 KiB | ROM，128 KiB | 地址/数据锁存与镜像 |
| 72、78、89 | 128 KiB | ROM，128 KiB | 72 为上升沿锁存；78 仅 submapper 1/3，拒绝歧义旧头及四屏；72/78 模拟总线冲突 |
| 75、92 | 256 KiB | ROM，128 KiB | 75 为 VRC1，无 IRQ；92 为上升沿锁存并模拟总线冲突 |
| 76、88 | 128 KiB | ROM，128 KiB | Namco 固定镜像，无 MMC3 IRQ 或交换模式 |
| 79 / 133 | 64 KiB | ROM，64 / 32 KiB | 扩展地址寄存器 |
| 86、244 | 128 KiB | ROM，64 KiB | 86 不模拟采样语音 |
| 87、184 | 固定 32 KiB | ROM，32 KiB | 独立 CHR 锁存 |
| 97 | 256 KiB | RAM，8 KiB | TAM-S1，上半区切换 PRG |
| 112 | 512 KiB | ROM，256 KiB | NTDEC；不接纳需要 CHR A18 的扩展容量 |
| 240 | 512 KiB | ROM，128 KiB | 扩展地址 PRG/CHR 锁存 |
| 242 | 512 KiB | RAM，8 KiB | 仅带 8 KiB 电池 RAM 的 Waixing RPG 板型；多合一卡变种未开放 |
| 243 | 128 KiB | ROM，128 KiB | SA-020A 寄存器读写与镜像；开放总线以地址高字节近似 |

- 72、75、86、92 的特殊控制器或采样语音不因 Mapper 准入而获得支持。
- 除 Mapper 0、1、3 外，PRG 不小于 32 KiB。

Mapper 69 的 `$6000–$7FFF` 窗口支持 ROM、启用 RAM 和禁用 RAM；ROM 映射不可写，禁用 RAM 的读取以地址高字节近似开放总线。IRQ 计数器按扫描线推进，计数与中断输出开关独立，不提供逐 CPU 周期精度。

准入列表定义于 `core_api/cartridge.hpp`，板型约束位于 `adapters/infones/cartridge.cpp`。核心中保留的其他 Mapper 实现不自动进入公共支持范围。扩展时需要同步补充板型约束与对应行为回归，见[架构中的扩展规则](architecture.md#扩展规则)。

`$6000–$7FFF` 的特殊访问由 Mapper 初始化时配置：`MapperReadSram` 非空时接管读取，`MapperSramOwnsWrite` 为真时由 `MapperSram` 独占写入并负责权限和脏标志。默认仍直接读取 `SRAMBANK`，写入后设置脏标志再通知 `MapperSram`。复位及卸载会清除特殊访问配置；新增板型应注册处理函数，不在 CPU 访存处追加 Mapper 编号判断。

### iNES

接收标准 NES 卡带头。拒绝带歧义保留字节、无法表达的 RAM 或硬件布局，不通过猜测修改 Mapper。Trainer、PRG 和 CHR 必须完整；文件长度与文件系统层的大小限制是两项独立检查。

### NES 2.0

保留原头，不转换成旧格式。支持范围如下：

- 标准 NES，NTSC / PAL / Dendy 时序，submapper 0（78 除外）、Mapper 2/3/7/25 的 submapper 2、Mapper 23 的 submapper 2/3、Mapper 78 的 submapper 1/3 或 Mapper 206 的 submapper 1；多区域声明默认选择 NTSC，核心与宿主帧时钟使用同一解析结果，保留原文件头。
- ROM 大小沿用上述限制，不支持扩展银行计数或指数容量编码。
- PRG RAM 总量不超过 8 KiB，不混合易失与非易失 PRG RAM；电池标志必须与 PRG NVRAM 声明一致。
- 无 CHR ROM 时，CHR RAM 必须为 8 KiB；不支持 CHR NVRAM 或 CHR ROM / RAM 混合。
- 多区域不提供运行时区域选择；不支持额外 ROM、特殊控制器及扩展硬件布局。

未知扩展 Mapper 明确拒绝，不降级为低八位。VS、PlayChoice、FDS 和 NSF 不在该加载接口的支持范围。

## 像素与显示

`line()` 接收 256 个 16 位像素，低六位为色号，bit 15 是核心使用的背景透明标志，**不是 RGB565**。适配层的 `NesPalette[64]` 使用 0–63 色号。核心已经完成背景与精灵合成，呈现层不把透明标志解释成颜色。

全部 240 行都会输出；背景与精灵左侧遮罩独立。`mask` 携带灰度和强调位，`pal` 提供颜色强调映射所需的区域信息。宿主必须在回调内消费或复制像素及这些状态；只保存六位色号会丢失强调信息。

设备呈现将每行转换成九位 token，再在显示核转换为 RGB565，详见[游戏与显示数据流](architecture.md#游戏与显示数据流)。宿主帧节奏和丢弃显示帧不改变 CPU / PPU / APU 的逻辑推进。

## 音频

`nes_host` 的音频桥接通过 `InfoNES_HasSoundOutput()` 声明没有输出设备，不生成基础波形缓冲、不求值或混合输出样本；APU / DMC 和扩展音源状态仍推进。脉冲波和三角波在寄存器事件处理后批量推进相位，噪声、DMC 和共享包络保持原有推进顺序。VRC7 依赖历史样本的 FM 合成沿用完整路径。音频设备输出需要单独实现底层声音钩子，`nes::Host` 没有音频回调。

底层 `InfoNES_System.h` 声音接口提供五路基础波形与可选扩展波形。`InfoNES_HasSoundOutput()` 在每次 APU 初始化时读取并锁定：返回 `false` 时不调用容量查询或输出回调；返回 `true` 时生成波形并按容量交付。瞬时队列容量和静音与设备能力独立，不能停止 APU / DMC；回调须同步消费或复制，不能保留借用指针。`SoundOpen` 返回 0 表示成功。声音初始化、打开和关闭钩子须容忍没有实际输出设备的情况。
