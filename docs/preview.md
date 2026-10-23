# 主机预览与 ROM 诊断

SDL2 客户端可操作实际游戏库和 Settings，预览 USB Disk 界面，启动游戏，查看错误并导出截图，无需连接设备或编译 Pico SDK。UI、索引、配置保存、输入仲裁、文件系统和 NES 核心使用项目原实现。

## 启动

Linux 需要主机编译工具、SDL2 2.0.18+ 开发包、dosfstools 和 mtools。例如 Debian / Ubuntu：

```sh
sudo apt install build-essential cmake ninja-build git python3 libsdl2-dev dosfstools mtools
python3 tools/preview.py
```

默认使用 `resources/`，也可选用包含 `rom/`、`system/`、`state/` 的完整本地资源目录：

```sh
python3 tools/preview.py --resources resources-local
python3 tools/preview.py --resources resources-local --settings
```

`--settings` 直接从 Settings 启动，`--disk` 直接从 USB Disk 启动，两者不能同时指定；默认从游戏库启动。所有界面也可正常通过菜单进入。

首次运行准备主机依赖和资源镜像，随后按资源内容变化刷新只读镜像。修改 ROM 后仍需使用索引工具更新该资源目录的 `system/roms/games.idx`；预览器与设备一样优先读取有效索引。`--no-build` 跳过编译，继续检查资源变化。

默认构建目录为 `build/preview/`。每个资源根目录有独立的 `sessions/<标识>/`，里面的 `state.bin` 是主机设置副本，退出 Settings 或正常关闭窗口时保存。不会回写 `resources*/` 或设备。`--reset-state` 恢复所选资源的初始配置；`--session DIR` 指定另一份主机配置。切换资源中的初始 State 文件不会自动覆盖已保存的主机设置。

窗口显示 320×240 原始画面，按整数倍放大，缩放窗口保持比例。操作如下：

| 按键 | 作用 |
|---|---|
| 方向键 | 游戏库切换/简介分页、Settings 导航、NES 方向 |
| Z / Enter | UI 确认 |
| X / Escape | UI 返回；游戏中为 NES B |
| Z / X | NES A / B |
| Enter / 右 Shift | NES Start / Select |
| Home | 退出当前应用回游戏库 |
| Q / W / E、I / K、H | 本机 A / B / C、Up / Down、Home，远程输入关闭时仍可用 |
| F2 | 切换模拟的 BLE / 串口 / 未连接输入来源 |
| F3 / F4 / F5 / F6 / F7 | USB Disk 预览内切换等待 / 传输 / 无效 FAT / I/O 错误 / 弹出 |
| F12 | 保存当前 320×240 画面到工作目录 `preview.bmp` |

默认由键盘模拟 BLE 手柄。蓝牙候选设备、电量、亮度硬件效果和 TE 状态为模拟值，不连接 PC 蓝牙或串口；关闭远程输入后可用本机映射继续操作。BOOTSEL 和休眠在完成真实设置保存流程后返回游戏库。NES 使用实际核心与区域时序，无声音输出；游戏进度不会保存。此工具不能用于判断 LCD、无线、Flash 寿命、设备性能或功耗。

USB Disk 的传输由按键模拟；F7 弹出后回到第一款游戏，按住本机 Home（H）会推迟返回，松开继续。Home 本身不退出磁盘模式。

### Windows

原生目标包含 Windows 内存映射适配，建议在 MSYS2 UCRT64 中使用 MinGW-w64 GCC、CMake、SDL2、Python，以及 dosfstools/mtools，使用 Ninja 生成器（例如设置 `CMAKE_GENERATOR=Ninja`）。当前自动化验证覆盖 Linux，Windows 构建与窗口尚未验证。MSVC 和跨编译不在当前验证范围。

也可在 Linux / WSL 中先生成 `rom.bin`、`system.bin`、`state.bin`，Windows 客户端以 `--images DIR` 读取其副本。直接运行客户端时，所选目录中的 `state.bin` 就是它的保存文件。

## 自动输入与截图

脚本为按时间顺序排列的 `帧编号 命令 参数`，从 0 开始。UI 和游戏每完成一帧推进一次编号；脚本模式不等待真实时间，UI 定时器每帧推进约 16.667 ms，游戏核心仍执行完整机器周期。实时游玩按 NTSC / PAL / Dendy 的实际帧期限运行。

例如保存为 `preview.txt`：

```text
0 source serial
2 shot library.bmp
3 press Left
4 release Left
6 press Return
7 release Return
10 expect view settings
10 shot settings.bmp
12 quit
```

```sh
python3 tools/preview.py --headless --script preview.txt
```

截图路径相对于脚本所在目录；输出 BMP 可直接查看。按键使用 SDL 名称，如 `Left`、`Return`、`Right Shift`。`press` 持续按住，必须用 `release` 松开；来源切换和页面切换保留实际的中立输入屏障。

| 命令 | 参数 |
|---|---|
| `press` / `release` | SDL 按键名 |
| `shot` | BMP 路径 |
| `source` | `ble` / `serial` / `none` |
| `battery` | 0–100；UI 按原采样周期刷新 |
| `save-failure` | `on` / `off`，模拟 State 写入失败 |
| `disk-state` | USB Disk 预览内使用 `ready` / `transferring` / `unavailable` / `failed` / `ejected` |
| `expect` | 字段和值，失败时返回非零退出码 |
| `quit` | 正常关闭并保存设置 |

断言字段：`view`（`library` / `settings` / `game` / `disk`）、`disk_state`（与 `disk-state` 的参数相同）、`page`（`app/settings.hpp` 的枚举值）、`selected`（0 为设置卡片）、`count`、`indexed`、`brightness`、`swap`、`te`、`dirty`、`controller`、`allowed`、`source`、`game_frames`。布尔值使用 `0` / `1`。同一帧的命令按书写顺序执行，断言和截图检查已完成的上一帧。脚本结束后自动退出，也可用 `--frames N` 限制总帧数。

运行预览集成回归；可选第二个目录保留截图和日志：

```sh
python3 tests/test_preview.py build/preview/host/tufty_preview .tmp/sdl-preview
ctest --test-dir build/preview -R '^host_preview$' --output-on-failure
```

集成回归覆盖默认资源、索引、设置保存/重载、TE 不持久化、保存失败重试、蓝牙列表、本机输入、游戏启动及 Home 返回，以及 USB Disk 入口、五种状态与弹出返回；独立临时目录运行，不覆盖日常主机配置。`--sanitize --build-dir build/preview-asan` 可另建带 ASan/UBSan 的客户端。

## 导出 HTML 报告

`tests/export_preview.py` 将现有绘图测试、SDL 集成截图和实际 ROM 核心画面汇总到一个可离线查看的 HTML。另需 Python Pillow（`python3 -m pip install Pillow`）。

```sh
python3 tests/export_preview.py
python3 tests/export_preview.py --resources resources-local --output .tmp/local-preview
```

默认 `--mode all`，自动在 `build/preview-export/` 编译所需主机目标，打开 `.tmp/preview-report/index.html` 查看。`--mode host` 只使用非 SDL 测试出口，无需 SDL；`--mode sdl` 使用 SDL 集成流程和所选资源的游戏库。两种模式都会逐个检查索引里的 ROM 并导出核心末帧。资源目录需包含有效的 `system/roms/games.idx`，不会自动重建索引或回写资源。

```sh
python3 tests/export_preview.py --mode host
python3 tests/export_preview.py --mode sdl --no-build --build-dir build/preview
python3 tests/export_preview.py --no-build --build-dir build/preview-asan --rom-frames 1020
```

`--no-build` 复用已有目标（也支持带 sanitizer 的构建），否则使用单独的无 sanitizer 报告构建。默认每款 ROM 跑 120 帧，从第 20 帧开始周期性按 Start；`--rom-frames` 调整帧数，`--timeout` 调整每个测试或 ROM 的秒数上限。加载器正常拒绝的 ROM 标记为 REJECT；崩溃、超时或断言失败会让导出返回非零，并保留已完成部分和日志。

报告按可视区域延迟加载 PNG，保留原始画面、输入脚本、日志和 `results.json`。每次生成独立的 `runs/` 子目录，入口指向最新报告，旧报告保留，可自行删除。Settings 绘图测试使用受控状态，SDL 集成检查点使用仓库默认四款资源；另列的游戏库和 ROM 画面使用 `--resources` 指定的资源。普通 CTest 不自动导出 HTML。当前完整导出已在 Linux 验证。

## ROM 诊断

`tests/rom_probe.cpp` 是无 SDL 的真实核心诊断程序，随普通主机测试构建：

```sh
bash tools/test.sh
build/host/tests/rom_probe resources/system/roms/TobuNES.nes --frames 120 --output .tmp/tobunes.ppm
```

它输出加载错误、完成帧数、末帧颜色 hash、服务调用数、未释放分配数及 ROM 是否被改写，返回非零表示失败。`--start-frame N` 从指定帧开始周期性按 Start，默认第 600 帧；`--frames` 上限为一百万。PPM 为最后一帧 256×240 的实际核心画面，输出父目录需存在。主机耗时仅用于主机比较。
