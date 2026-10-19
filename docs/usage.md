# 使用指南

## USB 接口

普通模式提供两个串口：

| 接口 | 名称 | 用途 |
|---|---|---|
| CDC0 | Console | 查看状态、修改设置、进入磁盘或 BOOTSEL 模式 |
| CDC1 | Gamepad | 接收手柄协议，由 PC 发送器使用 |

Console 设置：115200 波特率、关闭本地回显、启用 DTR。连接后输入 `help` 查看命令。支持退格和 Ctrl+C 清除输入。

普通模式 USB VID/PID 为 `CAFE:4025`，磁盘模式为 `CAFE:4027`。切换后串口编号可能改变，重新连接时请按接口名称选择。

## Console 命令

| 命令 | 用途 |
|---|---|
| `help` | 显示命令列表 |
| `status` | 查看文件系统、游戏数量及手柄状态 |
| `nes stats` | 查看游戏帧数、模拟/转换耗时、轮询间隔、LCD 耗时/丢帧与 TE 状态 |
| `brightness 1..100` | 调整屏幕亮度 |
| `padid 1..65535` | 设置串口及 TGP BLE 手柄 ID，默认 1；XTARK HID 不使用 ID |
| `swapab 0/1` | 关闭或启用 A/B 互换 |
| `pad status` | 查看活跃远程来源、各来源开关与输入可用状态 |
| `serial on` / `serial off` | 启用 / 关闭串口手柄，Console 始终可用 |
| `save` | 保存配置 |
| `rescan` | 返回菜单并重新扫描游戏 |
| `usb disk` | 进入 USB 磁盘模式 |
| `bootsel` | 进入刷机模式 |
| `power status` | 查看休眠设置、唤醒原因和最近一次错误 |
| `power sleep` | 进入深度休眠 |
| `power sleep 秒数` | 定时休眠，秒数为 1–4294967 |
| `power idle 秒数` | 设置菜单空闲休眠，0 关闭，范围 0–86400 |
| `ble status` | 查看 BLE 连接、按键、报告计数、最大服务间隔/耗时及恢复次数 |
| `ble on` / `ble off` | 启用 / 关闭 BLE |
| `ble scan` | 启用 BLE 并重新连接，保留目标地址设置 |
| `ble target auto` | 自动选择广播 HID 服务且报告布局兼容的设备 |
| `ble target MAC public` | 指定 public 地址目标 |
| `ble target MAC random` | 指定 random 地址目标 |
| `ble trace on` / `ble trace off` | 开关 HID 描述符诊断，默认关闭 |
| `ble discover` / `ble discover stop` | 限时发现附近设备 / 退出发现并恢复自动连接 |
| `ble devices` | 查看发现的 MAC、地址类型、名称、信号和 HID 标志 |
| `ble connect MAC public\|random` | 临时连接选中的设备，不修改白名单 |
| `ble allow MAC public\|random` | 添加白名单 |
| `ble prefer MAC public\|random` | 添加白名单并设为首选 |
| `ble forget MAC public\|random` | 删除白名单条目 |
| `ble whitelist` / `ble whitelist clear` | 查看 / 清空白名单 |
| `ble mode auto` / `ble mode whitelist` | 自动连接兼容设备 / 只连接白名单 |

修改设置后执行 `save`。启动不同游戏、通过命令进入磁盘模式前也会保存配置；保存失败时会显示错误。`usb disk`、`bootsel` 会先回复状态再切换。

串口与 BLE 同时可用时，最先成为活跃来源的手柄持续控制，另一来源保持连接但不混入按键。活跃来源禁用、断开或输入会话失效后才交接；接替的手柄需先松开按键、回中摇杆。HID 静置或 TGP 正常发送中立心跳时不触发交接。本机按键始终可用，使用 `pad status` 查看当前来源。

## 休眠与唤醒

菜单默认空闲 300 秒后深度休眠。实体按键、手柄按键和 Console 输入会重新计时；手柄连接和心跳消息不影响计时。游戏运行与 USB 磁盘模式不会自动休眠，从游戏返回菜单后重新计时。

```text
power idle 600
save
```

以上设置将菜单空闲时间改为 10 分钟。使用 `power idle 0` 并执行 `save` 可关闭自动休眠。

手动休眠可长按背面 Reset，保持至 LED 动画结束（启动后约 1.64 秒）；提前松开则正常启动。双 Reset 仍进入 USB 磁盘。Reset 操作会重启设备，修改配置后应先执行 `save`。

也可在菜单或游戏运行时输入 `power sleep`，或用 `power sleep 10` 测试 10 秒后自动唤醒。串口请求和菜单自动休眠都会先保存待写配置；保存失败或唤醒输入被按住时取消休眠。状态日志发送后才关闭屏幕和 USB。

按 A/B/C/Up/Down 可唤醒，Home 不用于唤醒。入睡时没有连接 USB，也可通过插入 USB 唤醒；已连接的 USB 不会让设备立即醒来。外部 RTC 已配置的闹钟信号也可触发唤醒，本固件不设置 RTC 闹钟。

唤醒后固件重新启动并进入菜单，游戏进度不保留。电脑串口会断开，Console 和 PC 手柄发送器需要重新连接。输入 `power status` 可查看 `BUTTON`、`ALARM`、`VBUS` 等唤醒原因。

## PC 手柄

Windows：

```powershell
python -m pip install pyserial
python tools/serial_gamepad.py
python tools/serial_gamepad.py --port COM7 --controller-id 1
python tools/serial_gamepad.py --port COM7 --v1
python tools/serial_gamepad.py --port COM7 --v2
```

设备 `padid` 必须与发送器的 `--controller-id` 相同。脚本默认使用 TGP v3，与 BLE HID 共用十字节按键 Report。`--v1`／`--v2` 切换为八字节协议，两者均发送完整八键，仅版本号不同，不支持 Esc/Home，返回菜单使用 Tufty 自身的 Home。详见下文 [TGP 模式](#tgp-v1v2-模式)。

发送器只在运行脚本的终端窗口处于前台时接收按键；切到其他窗口会自动释放全部按键和 Home，串口连接继续保持。Windows Terminal 请在前台启动脚本，建议使用独立窗口。v3 下 Esc 按下/松开发送 Home 状态；Ctrl+C 释放输入并结束会话。完整键位见 [README](../README.md#操作)。`--buttons` 固定按键测试不受窗口焦点影响，参数在所有版本中均为 NES 八键位图，由脚本转换为对应报文。

Linux 支持发送固定按键状态，不提供交互键盘输入：

```bash
python3 tools/serial_gamepad.py --port /dev/ttyACM1 --buttons 0x80 --seconds 3
```

## BLE 手柄

支持已适配的 XTARK 实体手柄，以及安卓手柄应用的 NES / XTARK HID 模式。

1. 开启实体手柄，或在安卓应用中选择 NES / XTARK HID 模式并点“开启手柄”。
2. Tufty 默认自动扫描连接。Console 输入 `ble status`，显示 `state=READY` 表示手柄就绪。
3. 十字键或左摇杆控制方向，A 启动游戏；游戏中使用 A/B、Select/Start。十字键优先于摇杆，右摇杆和肩键不映射 NES 操作。
4. 返回菜单时，将两个摇杆回中，松开其他按键及扳机，同时按下左右摇杆（L3+R3）并保持 **1.5 秒**。触发后松开两个摇杆按键，再进行下一步操作。安卓可直接使用 Home，也可按 Tufty 自身的 Home。

实体 XTARK 在 XTARK HID 模式下的 Home 键不发送游戏输入，使用上述组合键即可。串口与 BLE 可同时连接，由一个活跃远程来源控制；Tufty 自身按键始终可用。`swapab` 对合并后的 NES A/B 生效。

### 连接设置

附近有多个 HID 设备时，可通过 Console 固定实体手柄地址。连接日志会显示 MAC 和地址类型：

```text
ble target AA:BB:CC:DD:EE:FF public
save
```

安卓地址可能变化，建议使用自动连接：

```text
ble target auto
save
```

也可以先发现设备，再选择并保存白名单。`ble discover` 会断开当前 BLE 手柄并进入发现模式，约 10 秒后停止扫描，保留列表等待选择；发现期间不自动连接。用 `ble discover stop` 退出并恢复原连接策略。列表中的 HID 标志只表示广播服务，连接后仍需确认报文布局。

```text
ble discover
ble devices
ble prefer AA:BB:CC:DD:EE:FF public
ble mode whitelist
ble connect AA:BB:CC:DD:EE:FF public
save
```

请替换成发现的地址和类型。白名单最多四项，首选设备有两秒扫描优先窗口，未发现时允许其他白名单设备连接。`ble target MAC public|random` 是替换为单设备白名单的快捷方式；`ble target auto` 保留列表并恢复自动模式。清空白名单不会自动切换模式，白名单模式下列表为空就不自动连接。

设置统一保存到 `/state/nes.json`，磁盘中的 `config/gamepad.json` 不生效。安卓地址可能轮换，固定某次随机 MAC 不能保证重启后仍可连接，建议使用自动模式。BLE HID 不使用串口的 `padid`。`serial off` 只关闭手柄会话，不影响 Console。

断连后会自动等待重连。安卓点“关闭手柄”后 Tufty 会断开并继续扫描，重新开启即可连接，无需关闭手机系统蓝牙。长时间不操作不会触发 BLE 超时重连；游戏与菜单空闲休眠规则仍然生效。休眠会关闭无线，需要用 Tufty 实体按键唤醒。USB 磁盘模式不启用手柄连接。

### TGP v1/v2 模式

TGP v1/v2 均支持串口和 BLE 连接。默认 ID 为 1，修改时两端保持一致，用 `padid` 设置 Tufty，再 `save`。BLE 自动识别 TGP 服务；若之前固定了其他 MAC，可用 `ble target auto` 恢复自动连接。

早期 TGP v1 发送器只有方向和 A；v2 增加 B、Select、Start，本固件对 v1/v2 均按完整八键接收。两者均没有 Home，返回菜单使用 Tufty 自身的 Home。TGP 需要 120 ms 心跳；BLE 订阅就绪后 350 ms 无首帧，或此后 350 ms 无有效状态，会释放输入、取消订阅并断开连接，随后自动扫描重连。HID 静置不受此超时影响。串口仍在 350 ms 释放输入、1200 ms 结束会话，发送器连接 CDC1，Console 使用 CDC0。本仓库脚本默认 v3，也可用 `--v1`／`--v2`。报文及握手见 [手柄协议](gamepad-protocol.md#tgp-v1v2)。

需要重新连接时使用 `ble scan`；`ble off` / `ble on` 关闭或启用 BLE，执行 `save` 保存开关和目标设置。若无法连接，先检查目标地址和安卓模式，再用 `ble status` 查看状态。`ble trace on` 后执行 `ble scan` 可获取 HID 描述符诊断，完成后用 `ble trace off` 关闭。

HID 模式支持 XTARK 十字节 Report 布局，其他 HID 布局需单独适配。串口与 BLE 接入约定见 [手柄协议](gamepad-protocol.md)。

## 管理游戏

通过 Console 的 `usb disk` 或连续双 Reset 进入磁盘模式，把游戏放到盘根目录 `roms/`。完成后使用电脑的安全弹出操作，设备自动返回菜单。磁盘模式下暂停游戏、手柄和 Console 命令。

磁盘界面显示星空和操作提示。开始写入时先显示一帧 `Transferring data!`，随后冻结画面、四颗背灯常亮；最后一次写入完成 500 ms 后切回 `Waiting for data` 并熄灯。读取文件、扫描目录只暂停动画，不触发传输状态或亮灯，也不延长写入指示；读写都停止 500 ms 后恢复动画。电脑后台写入磁盘元数据也会触发写入指示。即使背灯已熄灭，也应先在电脑上安全弹出磁盘。

游戏文件要求：

- 标准 iNES 1.0 或兼容的 NES 2.0 `.nes` 文件，单个文件最多 4 MiB。
- 只扫描 `roms/` 第一层，最多 128 个游戏。
- 文件名最多 255 个 UTF-8 字节，菜单按文件名排序。
- 菜单字体只显示 ASCII，建议使用英文文件名及两位数字前缀。

公共加载入口支持 Mapper 0、1、2、3、4、7、48、66 的常规板型布局，PRG 最多 512 KiB、CHR 最多 256 KiB，均须为二的幂并满足板型限制。其余 Mapper 实现保留在核心中，不属于公共加载入口的支持范围。

NES 2.0 支持标准 NES、明确的 NTSC/PAL/Dendy 时序、submapper 0 和受限 RAM，不转换成旧头，也不修改原文件。完整尺寸、RAM 和外设限制见 [核心契约](core-contract.md)。未知扩展 Mapper 明确报错，不回退为低八位 Mapper。

重命名游戏后，请删除设备上的旧文件以免重复显示；上次选择会在启动新文件后更新。

## ROM 检查与修复

将电脑上的游戏放到仓库根目录 `roms/`，执行：

```bash
python3 tools/deps.py --profile host
python3 tools/prepare_roms.py
python3 tools/prepare_roms.py --fix
python3 tools/prepare_roms.py --fix --trim-padding
```

默认只检查。`--fix` 按已知校验值修正头部，修改前将完整原文件备份到 `build/rom-backups/`；用 `--backup-dir` 可更换备份位置。`--trim-padding` 额外清理声明 ROM 数据之外的 ASCII 空白尾部。处理完成后，将游戏手动复制到设备。

检查项包括格式、文件长度、mapper、大小限制和尾部数据。未知 ROM 不修改头部，截断或不支持的游戏会报错。修正规则位于 `tools/rom_fixes.json`，可用 `--rules` 指定自定义规则。规则只能处理已知文件问题，不能保证所有游戏兼容。

## 封面和背景

素材使用无头、小端 RGB565 格式，放在设备磁盘内：

| 路径 | 尺寸 | 大小 |
|---|---|---|
| `roms/Game.rgb565` | 60 × 72 | 8640 字节 |
| `assets/nes-background.rgb565` | 320 × 200 | 128000 字节 |

封面应与游戏使用相同文件名主干，如 `01_Game.nes` 和 `01_Game.rgb565`。无封面时显示默认卡带；无自定义背景时依次使用 `/rom/nes-background.rgb565` 或内置背景。
