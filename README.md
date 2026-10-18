# tufty-infonesPlus

面向 Tufty2350 的独立 NES 固件。

当前提供可单独构建的卡带模拟核心和主机回归。保留 CPU、PPU、APU、
区域选择、卡带 Mapper 与扩展音频。Tufty 硬件与业务将在独立层接入。

## 主机编译与测试

需要 CMake 3.21+、Ninja、支持 C++17 的 GCC/Clang，不需要 Pico SDK。

```sh
bash tools/test.sh
```

默认使用 ASan/UBSan。独立构建核心：

```sh
cmake -S . -B build/core -G Ninja -DBUILD_TESTING=OFF -DNES_SANITIZERS=OFF
cmake --build build/core
```

`nes_core` 是单实例静态库，不拥有宿主窗口、文件系统、无线或菜单。
调用方提供 `InfoNES_System.h` 的显示/输入/声音回调与
`InfoNES_Port.h` 的分配、选项和每扫描线服务接口。像素/波形缓冲为借用视图，
消费方必须在回调返回前完成复制。不要把 `InfoNES_Main(int region)` 的参数
当成旧 pico-nes 的 `bool skip_fb`。

当前代码继承扫描线模拟及上游兼容近似；Mapper 表存在某编号不等于完整板型验证，
NES 2.0 文件头识别也不等于完整格式支持。测试只覆盖注明的用例，尚无本分支设备结论。

应用使用 `nes_cartridge` 的受检加载入口。生命周期、初期 Mapper/NES 2.0
支持范围和像素/音频约定见 [核心契约](docs/core-contract.md)。

## 依赖

设备依赖通过 `deps.lock.json` 固定版本，使用 `python3 -B tools/deps.py --radio` 准备。
依赖检查和离线构建说明见 [构建](docs/building.md)。

## 许可与核心来源

本项目采用 [GPL-3.0](LICENSE)。InfoNES 核心取自
[pico-infonesPlus](https://github.com/PicoPlus-devel/pico-infonesPlus)
v0.54（`eed95b61edd6d31ed0fc30410afad3e9a5e709ca`），在此基础上独立修改与维护。
源码内原有版权及许可声明予以保留。
