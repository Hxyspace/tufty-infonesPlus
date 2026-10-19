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

公共加载入口接纳 Mapper **0、1、2、3、4、7、48、66**，并施加以下约束：

- PRG / CHR ROM 容量为二的幂，PRG 为 16–512 KiB，存在 CHR ROM 时为 8–256 KiB。
- Mapper 0（NROM）：PRG 不超过 32 KiB，CHR ROM 不超过 8 KiB。
- Mapper 3（CNROM）：PRG 不超过 32 KiB，必须有 CHR ROM 且不超过 32 KiB。
- Mapper 2（UxROM）与 Mapper 7（AxROM）：仅使用 CHR RAM。
- Mapper 48 与 66：必须有 CHR ROM。
- 除 Mapper 0、1、3 外，PRG 不小于 32 KiB。

准入列表定义于 `core_api/cartridge.hpp`，板型约束位于 `adapters/infones/cartridge.cpp`。核心中保留的其他 Mapper 实现不自动进入公共支持范围。扩展时需要同步补充板型约束与对应行为回归，见[架构中的扩展规则](architecture.md#扩展规则)。

### iNES

接收标准 NES 卡带头。拒绝带歧义保留字节、无法表达的 RAM 或硬件布局，不通过猜测修改 Mapper。Trainer、PRG 和 CHR 必须完整；文件长度与文件系统层的大小限制是两项独立检查。

### NES 2.0

保留原头，不转换成旧格式。支持范围如下：

- 标准 NES，明确的 NTSC / PAL / Dendy 时序，submapper 0。
- ROM 大小沿用上述限制，不支持扩展银行计数或指数容量编码。
- PRG RAM 总量不超过 8 KiB，不混合易失与非易失 PRG RAM；电池标志必须与 PRG NVRAM 声明一致。
- 无 CHR ROM 时，CHR RAM 必须为 8 KiB；不支持 CHR NVRAM 或 CHR ROM / RAM 混合。
- 不支持多区域自动选择、额外 ROM、特殊控制器及扩展硬件布局。

未知扩展 Mapper 明确拒绝，不降级为低八位。VS、PlayChoice、FDS 和 NSF 不在该加载接口的支持范围。

## 像素与显示

`line()` 接收 256 个 16 位像素，低六位为色号，bit 15 是核心使用的背景透明标志，**不是 RGB565**。适配层的 `NesPalette[64]` 使用 0–63 色号。核心已经完成背景与精灵合成，呈现层不把透明标志解释成颜色。

全部 240 行都会输出；背景与精灵左侧遮罩独立。`mask` 携带灰度和强调位，`pal` 提供颜色强调映射所需的区域信息。宿主必须在回调内消费或复制像素及这些状态；只保存六位色号会丢失强调信息。

设备呈现将每行转换成九位 token，再在显示核转换为 RGB565，详见[游戏与显示数据流](architecture.md#游戏与显示数据流)。宿主帧节奏和丢弃显示帧不改变 CPU / PPU / APU 的逻辑推进。

## 音频

`nes_host` 的音频桥接报告零容量，不交付声音；APU / DMC 仍推进。音频设备输出需要单独实现底层声音钩子，`nes::Host` 没有音频回调。

底层 `InfoNES_System.h` 声音接口提供五路基础波形与可选扩展波形。容量与静音只改变样本交付，不能停止 APU / DMC；回调须同步消费或复制，不能保留借用指针。无输出设备时可报告零容量并提供空消费回调，`SoundOpen` 返回 0 表示成功。
