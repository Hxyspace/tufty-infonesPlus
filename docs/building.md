# 构建指南

## 工具要求

构建脚本面向 Bash 环境。工具要求如下：

| 用途 | 工具 |
|---|---|
| 主机编译与测试 | CMake 3.21+、Ninja、Git、Python 3.10+、支持 C++17 的 GCC 或 Clang |
| Tufty2350 固件 | 上述工具，以及 `arm-none-eabi-gcc` / `arm-none-eabi-g++` |
| 三种镜像打包及完整文件系统测试 | `mkfs.fat`（dosfstools）、`mmd` / `mcopy`（mtools） |
| PC 手柄发送器 | Python 3、pyserial，见[使用指南](usage.md#pc-手柄) |
| 游戏索引工具 | Python 3.10+；交互排序需要 curses 终端，`--yes` 无需 curses |

源码使用 C11 / C++17。主机测试默认开启 AddressSanitizer 和 UndefinedBehaviorSanitizer。设备固件使用 Release 配置，主机与设备使用不同构建目录。

## 依赖管理

外部依赖的来源、完整 commit 和补丁由 `deps.lock.json` 固定，下载到本地 `lib/`，不作为顶层 Git submodule 提交。InfoNES 核心直接在本仓库维护。

设备构建脚本自动准备依赖，也可以手动准备或检查：

```sh
python3 -B tools/deps.py --radio
python3 -B tools/deps.py --radio --check
```

`--check` 只验证版本和补丁，不下载、不修改依赖。准备器遇到错误版本或额外修改会报错并保留现场，不会自动 reset 或覆盖。SDK 内的 TinyUSB、CYW43、BTstack 使用 SDK 固定的子模块版本。依赖准备齐全后可离线构建。

仅准备主机业务测试的依赖：

```sh
python3 -B tools/deps.py --profile host --radio
```

`--profile host` 准备文件系统和 JSON 库，`--radio` 额外准备 BLE 测试所需组件。修改依赖版本或补丁时，同时更新锁文件及其校验信息；CMake 配置只验证依赖，不负责下载。

## 构建固件

在仓库根目录运行：

```sh
bash tools/build.sh
```

脚本检查工具并准备依赖，使用主机编译器构建 picotool，再使用 ARM 编译器构建固件，检查 UF2 地址范围并打包。

| 产物 | 内容 |
|---|---|
| `dist/tufty-nes-flash.uf2` | 仅程序，保留设备资源和配置分区 |
| `dist/tufty-nes-with-romfs.uf2` | 程序＋完整替换 /rom，只读背景随此镜像部署 |
| `dist/tufty-nes-with-filesystem.uf2` | 程序＋/rom＋/system＋LittleFS，初始化或恢复出厂资源，覆盖游戏和配置 |
| `dist/rom.bin`、`system.bin`、`state.bin` | 原始分区镜像，用于验证和高级工具；不直接拖入 BOOTSEL |
| `dist/resource-images.json` | 资源分区大小、偏移、哈希与各镜像覆盖范围 |
| `dist/tufty-nes-flash.elf` | 带符号的固件，用于调试和分析 |
| `dist/build-info.json` | 源码版本、依赖版本、补丁摘要和产物 SHA-256 |
| `dist/README.md`、`dist/docs/`、`dist/licenses/` | 使用文档与许可证 |

普通固件内容限制在 Flash 前 2 MiB，UF2 另保留 SDK 的 RP2350 E10 兼容块。刷写行为与文件系统要求见[存储与升级](storage.md)。构建脚本不自动刷机。

进入设备 BOOTSEL 模式后，将 UF2 复制到设备的启动磁盘；已经运行本固件时，也可通过 Console 输入 `bootsel`。默认资源不附带游戏 ROM，完整镜像包含可直接挂载的文件系统。

## 主机测试

核心、宿主适配、呈现逻辑及依赖保护测试不需要 Pico SDK，也不需要联网：

```sh
bash tools/test.sh
```

完整业务测试还覆盖 BLE、USB、FAT / LittleFS、菜单、电源及主机工具：

```sh
python3 -B tools/deps.py --profile host --radio
bash tools/test.sh --business
```

使用 Clang 时单独指定构建目录，避免复用 GCC 的 CMake 缓存：

```sh
CC=clang CXX=clang++ TUFTY_TEST_BUILD_DIR="$PWD/build/host-clang" bash tools/test.sh --business
```

主机测试用于验证可重复的核心与业务逻辑。LCD 传输、无线连接、Flash 写入和休眠唤醒等硬件行为需要在设备上验证；运行诊断见[使用指南](usage.md#console-命令)。

## 独立构建核心

只生成核心与宿主适配静态库：

```sh
cmake -S . -B build/core -G Ninja -DBUILD_TESTING=OFF -DNES_SANITIZERS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/core
```

`nes_core` 提供模拟器，`nes_cartridge` 提供卡带检查与加载，`nes_host` 提供同步宿主回调桥接。集成入口和生命周期见[核心契约](core-contract.md)。

## 构建选项

| 环境变量 | 默认值 | 用途 |
|---|---|---|
| `TUFTY_BUILD_DIR` | `build/firmware` | 设备构建目录 |
| `TUFTY_ARTIFACT_DIR` | `dist` | 固件打包目录 |
| `TUFTY_TEST_BUILD_DIR` | `build/host` | 主机测试构建目录 |
| `CMAKE_BUILD_PARALLEL_LEVEL` | `4` | 并行构建任务数 |
| `PYTHON` | `python3` 的路径 | Python 解释器 |

切换编译器、目标架构或构建类型时使用不同构建目录。依赖校验失败时，根据报错检查 `lib/` 中对应库的版本和本地修改，再决定保留修改或重新准备依赖。


## 资源镜像

`resources/rom`、`resources/system`、`resources/state` 分别是 RM1、FAT 和 LittleFS 的构建输入，目录映射见 [资源说明](../resources/README.md)。运行构建脚本始终生成三种镜像，不通过改源码切换版本。

RM1 使用内联文件数据编码，由固件实际 RM1 读取器验证。FAT 使用 dosfstools/mtools 构建 12 MiB FAT12 卷、4096 字节扇区。LittleFS 使用项目锁定版本的库和主机 C 编译器构建，几何参数与设备一致（256 字节读写、4096 字节块、256 块），不依赖另一个 Python 文件系统库。可用 `HOST_CC` 指定主机 C 编译器。

打包器校验输入固件 UF2，再加入资源块并统一重新编号；保留 SDK 独立的兼容块。带资源的 UF2 默认采用 sparse 打包：只写入文件系统元数据和文件占用的 4 KiB 擦除扇区，每个选中扇区保留完整的 16 个 UF2 数据页。RM1 根据有效记录范围、FAT 根据分配表、LittleFS 根据构建时实际使用的块确定范围，不会因文件内容为全零或全 `0xFF` 而省略它。固件＋ROM 版不会包含 system/State 的普通数据块。合并后检查块头、覆盖范围、重复地址及每块数据。标准完整业务测试还会模拟空白及已有数据的 Flash，使用实际固件代码挂载三个文件系统并保存配置。

UF2 文件大小随实际资源量变化；未使用的分区空间不占 UF2 数据块，但分区容量不变。原始 `rom.bin`、`system.bin`、`state.bin` 仍保留完整分区大小，适合离线检查。`resource-images.json` 记录分区大小、实际写入字节数、相对分区起点的擦除扇区偏移及 SHA-256，全部产物哈希记录在 `build-info.json`。构建工具只处理本地文件，不自动写设备。

完整文件系统镜像会重建文件目录及配置，原文件不再可访问；sparse 打包不擦除未使用扇区中的旧数据，不属于安全擦除。无需在刷入前清空设备，文件系统在后续分配这些空间时会正常复用。
