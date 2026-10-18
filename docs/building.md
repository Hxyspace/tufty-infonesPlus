# 构建与依赖

当前核心主机回归不需要 SDK 或联网：`bash tools/test.sh`。
CMake/Ninja、GCC 或 Clang、Python 3 是主机工具；设备构建还需
`arm-none-eabi-gcc/g++`。主机调试默认开启 ASan/UBSan。

设备与完整业务测试的依赖通过 `python3 -B tools/deps.py --radio` 显式准备，
`python3 -B tools/deps.py --radio --check` 只验证，不下载、不应用补丁。
`--profile host` 仅准备 FatFS/LittleFS/cJSON，BLE 主机测试再加 `--radio`。
所有依赖在 `lib/`，未提交到本仓库；每个来源和完整 commit 在 `deps.lock.json`。
SDK 内 TinyUSB/CYW43/BTstack 的版本由 SDK gitlink 决定。

准备器保留不符版本、暂存或额外修改并报错，不会静默 reset/覆盖。
补丁按临时索引计算预期树；仅接受与锁定源码加补丁完全一致的结果。
FatFS 镜像保留 CRLF，应用补丁时允许上下文空白差异，但最终树仍严格核对。
core 直接维护在本仓库，不在构建时下载另一个 NES 工程。

独立 FatFS R0.14b 与此前设备项目版本相同；只有外部配置头补丁。
业务配置为只读 FAT，支持 512/4096 字节扇区和 UTF-8 长文件名。
Flash 写入和 USB MSC 独占策略由业务层实现，不通过 FatFS 直接写 ROM 分区。

设备目标及打包命令在硬件接入阶段提供；当前不能把主机静态库刷入设备。
