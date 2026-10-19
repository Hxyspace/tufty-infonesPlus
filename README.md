# tufty-infonesPlus

面向 Tufty2350 的独立 NES 模拟器固件，包含 InfoNES 核心、硬件驱动、游戏菜单、手柄连接和存储管理。

## 功能

- CPU、PPU、APU 与卡带 Mapper 模拟，支持 NTSC、PAL、Dendy 时序。
- 双核显示：模拟与应用运行在 core0，LCD 传输运行在 core1，使用 PSRAM 帧缓冲和 PIO/DMA 输出。
- 游戏列表、封面与自定义背景，通过 USB 磁盘管理游戏和素材。
- 本机按键、XTARK / 安卓 HID 手柄、USB 串口 TGP v3，以及串口和 BLE TGP v1/v2。
- Console 配置、BLE 设备发现与白名单、亮度调节、自动休眠与按键唤醒。

仅适配 Tufty2350。公共加载入口支持 Mapper **0、1、2、3、4、7、48、66、69** 的受限板型布局，以及受限 NES 2.0 格式；详细范围见[核心契约](docs/core-contract.md#卡带支持范围)。固件没有音频设备输出，不保存游戏进度或电池 SRAM；FDS、NSF 和特殊外设不在支持范围内。

## 开始使用

1. 按[构建指南](docs/building.md)生成 `dist/tufty-nes-flash.uf2`。
2. 确认设备符合[存储布局](docs/storage.md#flash-布局)，进入 BOOTSEL 模式并复制 UF2。固件包只包含程序，不包含文件系统镜像，也不自动格式化 Flash。
3. 启动后通过连续双 Reset，或 Console 的 `usb disk` 命令进入磁盘模式，将自己的 `.nes` 文件放入磁盘 `roms/` 目录，安全弹出后返回菜单。
4. 连接手柄，选择游戏并启动。连接方法、Console 命令和素材格式见[使用指南](docs/usage.md)。

设备构建：

```sh
bash tools/build.sh
```

主机测试不需要 Pico SDK：

```sh
bash tools/test.sh
```

工具要求、完整业务测试和离线构建说明见[构建指南](docs/building.md)。仓库与固件包不附带游戏 ROM。

## 操作

| 操作 | Tufty 本机按键 | PC 发送器 |
|---|---|---|
| 菜单上一项 / 下一项 | A 或 Up / C 或 Down | 方向键或 WASD |
| 启动游戏 | B | Z 或 Space |
| NES A / B | B / 由手柄提供 | Z 或 Space / X |
| Start / Select | 由手柄提供 | Enter / 右 Shift |
| 游戏上 / 下 / 左 / 右 | Up / Down / A / C | 方向键或 WASD |
| 返回菜单 | Home | Esc（TGP v3） |

HID 手柄使用 Home，或摇杆回中并同时按住 L3+R3 1.5 秒返回菜单。TGP v1/v2 使用本机 Home。串口与 BLE 同时连接时使用一个活跃远程来源，本机按键始终可用。

## 文档

| 文档 | 内容 |
|---|---|
| [使用指南](docs/usage.md) | USB、手柄、Console、休眠、游戏与素材管理 |
| [构建指南](docs/building.md) | 工具链、固定依赖、固件打包与主机测试 |
| [固件架构](docs/architecture.md) | 目录职责、模块关系、双核协作和扩展边界 |
| [核心契约](docs/core-contract.md) | 宿主 API、卡带加载、支持范围与资源生命周期 |
| [存储与升级](docs/storage.md) | Flash 分区、配置格式、磁盘独占和升级注意事项 |
| [手柄协议](docs/gamepad-protocol.md) | TGP 报文、HID 布局、会话与超时规则 |

## 许可与核心来源

本项目采用 [GPL-3.0](LICENSE)。InfoNES 核心取自
[pico-infonesPlus](https://github.com/PicoPlus-devel/pico-infonesPlus)
v0.54（`eed95b61edd6d31ed0fc30410afad3e9a5e709ca`），在此基础上独立修改与维护。
源码内原有版权及许可声明予以保留；字体与其他素材许可见 `licenses/`。
