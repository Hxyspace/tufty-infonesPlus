# tufty-infonesPlus

面向 Tufty2350 的独立 NES 模拟器固件，包含 InfoNES 核心、硬件驱动、游戏菜单、手柄连接和存储管理。

## 功能

- CPU、PPU、APU 与卡带 Mapper 模拟，支持 NTSC、PAL、Dendy 时序。
- 双核显示：模拟与应用运行在 core0，LCD 传输运行在 core1；游戏帧使用 SRAM，菜单帧使用 PSRAM，通过 PIO/DMA 输出。
- 带顺序、英文标题和分页简介的游戏索引，58×80 封面与自定义背景，通过 USB 磁盘管理游戏和素材。
- 本机按键、XTARK / 安卓 HID 手柄、USB 串口 TGP v3，以及串口和 BLE TGP v1/v2。
- 图形 Settings，支持 BLE 设备发现与允许列表、亮度调节、自动休眠与按键唤醒。

仅适配 Tufty2350。公共加载入口支持 Mapper **0、1、2、3、4、7、48、66、69** 的受限板型布局，以及受限 NES 2.0 格式；详细范围见[核心契约](docs/core-contract.md#卡带支持范围)。固件没有音频设备输出，不保存游戏进度或电池 SRAM；FDS、NSF 和特殊外设不在支持范围内。

## 开始使用

1. 按[构建指南](docs/building.md)生成三种 UF2：程序更新版、程序＋只读资源版、完整初始化版。
2. 进入 BOOTSEL 并复制所需 UF2：空白设备使用 `tufty-nes-with-filesystem.uf2` 初始化；已有设备普通更新使用 `tufty-nes-flash.uf2`，同时更新只读背景使用 `tufty-nes-with-romfs.uf2`。**完整初始化版会覆盖原有游戏、素材和配置**，详见[存储布局](docs/storage.md#flash-布局)。
3. 通过游戏库最左侧的 `Settings → System → USB disk mode` 或连续双 Reset 进入磁盘模式，将自己的 `.nes` 文件放入磁盘 `roms/` 目录，安全弹出后返回菜单。
4. 连接手柄，选择游戏并启动。连接方法和素材格式见[使用指南](docs/usage.md)。

设备构建：

```sh
bash tools/build.sh
```

主机测试不需要 Pico SDK：

```sh
bash tools/test.sh
```

无需刷机的交互预览：`python3 tools/preview.py`。支持游戏库、Settings、USB Disk 界面和游戏画面，准备方式与按键见[主机预览](docs/preview.md)。

工具要求、完整业务测试和离线构建说明见[构建指南](docs/building.md)。完整文件系统镜像附带 Chase、LAN Master、TobuNES、Cargo Shift 四款游戏及菜单资源，刷入后可直接体验；自定义镜像资源见 [resources](resources/README.md)。

可使用 `python3 tools/build_game_index.py` 预览、排序并生成游戏索引；`--yes` 无交互生成，`--output deploy/roms` 创建新的设备包。将包内 ROM、`.565` 封面和 `games.idx` 复制到设备磁盘的 `roms/`，详见[索引与部署](docs/usage.md#游戏索引封面和背景)。

## 操作

| 操作 | Tufty 本机按键 | PC 发送器 |
|---|---|---|
| 菜单上一项 / 下一项 | A / C | 左 / 右 |
| 菜单简介上一页 / 下一页 | Up / Down | 上 / 下 |
| 启动游戏 | B | Z 或 Space |
| NES A / B | B / 由手柄提供 | Z 或 Space / X |
| Start / Select | 由手柄提供 | Enter / 右 Shift |
| 游戏上 / 下 / 左 / 右 | Up / Down / A / C | 方向键或 WASD |
| 退出当前应用回首页 | Home | Esc（TGP v3） |

HID 手柄使用 Home，或摇杆回中并同时按住 L3+R3 1.5 秒返回菜单。TGP v1/v2 使用本机 Home。串口与 BLE 同时连接时使用一个活跃远程来源，本机按键始终可用。

## 文档

| 文档 | 内容 |
|---|---|
| [使用指南](docs/usage.md) | 手柄、休眠、游戏与素材管理，以及开发调试命令 |
| [构建指南](docs/building.md) | 工具链、固定依赖、固件打包与主机测试 |
| [主机预览](docs/preview.md) | SDL2 客户端、自动输入/截图与 ROM 诊断 |
| [固件架构](docs/architecture.md) | 目录职责、模块关系、双核协作和扩展边界 |
| [核心契约](docs/core-contract.md) | 宿主 API、卡带加载、支持范围与资源生命周期 |
| [存储与升级](docs/storage.md) | Flash 分区、镜像选择、磁盘访问和升级注意事项 |
| [手柄协议](docs/gamepad-protocol.md) | TGP 报文、HID 布局、会话与超时规则 |

## 许可与核心来源

本项目采用 [GPL-3.0](LICENSE)。InfoNES 核心取自
[pico-infonesPlus](https://github.com/PicoPlus-devel/pico-infonesPlus)
v0.54（`eed95b61edd6d31ed0fc30410afad3e9a5e709ca`），在此基础上独立修改与维护。
源码内原有版权及许可声明予以保留；字体与其他素材许可见 `licenses/`。
