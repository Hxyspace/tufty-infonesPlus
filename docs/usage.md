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
| `menu stats` | 查看菜单资源准备、绘制及含显示等待的帧耗时（微秒） |
| `nes stats` | 查看游戏/LCD 区间 FPS、模拟/发布/服务耗时、TE 周期/等待、游戏帧年龄、替换帧和内存统计 |
| `lcd te` | 查询 TE 同步的请求状态、实际状态及累计超时次数 |
| `lcd te on` / `lcd te off` | 开启 / 关闭 LCD 发送前的 TE 等待；默认开启，仅本次运行有效 |
| `brightness 1..100` | 调整屏幕亮度 |
| `padid 1..65535` | 设置串口及 TGP BLE 手柄 ID，默认 1；XTARK HID 不使用 ID |
| `swapab 0/1` | 关闭或启用 A/B 互换 |
| `pad status` | 查看活跃远程来源、各来源开关与输入可用状态 |
| `serial on` / `serial off` | 启用 / 关闭串口手柄，Console 始终可用 |
| `save` | 保存配置 |
| `rescan` | 返回菜单并强制扫描目录（本次跳过索引） |
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

修改设置后执行 `save`。通过命令进入磁盘模式前也会保存配置；保存失败时会显示错误。`usb disk`、`bootsel` 会先回复状态再切换。

`lcd te on/off` 可在菜单或游戏中切换，当前显示帧完成后生效，不需要退出游戏。该开关不写入配置，`save` 不保存它，重启恢复开启。命令回复的 `requested` 是请求值，`active` 是显示核的实际状态，切换瞬间可能不同；稍后用 `lcd te` 或 `nes stats` 确认。关闭 TE 可减少同步等待，但可能出现画面撕裂；它不改变模拟时钟、分辨率或颜色。TE 超时后会自动降级，`lcd te on` 可重新尝试同步，累计超时次数保留。

连续两次查询 `nes stats` 可获取 LCD 区间 FPS；首次只建立显示统计窗口。`game_fps` 只统计游戏画面传输，`LCD fps` 包含菜单。`drops` 表示待显示旧游戏帧被更新帧替换，不代表跳过了模拟；详细字段定义见[固件架构](architecture.md#扩展规则)。

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
- 只读取 `roms/` 第一层，最多 128 个游戏。
- 文件名最多 255 个 ASCII 字节，扩展名 `.nes` 不区分大小写。
- 菜单优先读取 `roms/games.idx` 中的顺序、标题、简介和封面标志；索引缺失或损坏时回退为目录扫描并按文件名排序。
- 正常索引不会在设备上重新排序。新增、删除或改名后，在电脑上重新生成索引。
- 启动游戏时才读取并校验 ROM 内容；菜单目录阶段不读取 ROM 数据。

公共加载入口支持 Mapper 0、1、2、3、4、7、48、66、69 的常规板型布局，PRG 最多 512 KiB、CHR 最多 256 KiB。PRG 容量须为二的幂，CHR ROM 除 Mapper 4 允许按 8 KiB 追加银行外也须为二的幂，并满足板型限制。其余 Mapper 实现保留在核心中，不属于公共加载入口的支持范围。

NES 2.0 支持标准 NES、明确的 NTSC/PAL/Dendy 时序、submapper 0 和受限 RAM，不转换成旧头，也不修改原文件。完整尺寸、RAM 和外设限制见 [核心契约](core-contract.md)。未知扩展 Mapper 明确报错，不回退为低八位 Mapper。

重命名游戏后，请删除设备上的旧文件并重新生成 `games.idx` 以免重复显示。菜单每次启动从索引第一项开始；当前选择只保存在运行期内。封面下显示游戏位置，多页简介显示页码和上下三角；首末页对应方向变暗。本机 A/C 和手柄左右切游戏，上下翻简介，长按 400 ms 后每 150 ms 重复一次，到边界停住。确认键只响应按下边沿。启动失败时保留卡片并显示简短英文原因，松开后重新按确认键可重试，切换到其他游戏后清除错误。

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

## 游戏索引、封面和背景

`tools/build_game_index.py` 从工作目录生成 `games.idx`。工作目录可以包含 ROM、同名 `.565` 封面、同名 `.TXT` 简介和 PC 使用的 `game-order.json`：

```bash
# 交互预览并排序，原地更新 roms/games.idx
python3 tools/build_game_index.py

# 无交互重建
python3 tools/build_game_index.py --input roms --yes

# 生成全新的设备包目录（目录必须不存在）
python3 tools/build_game_index.py --input roms --output deploy/roms --yes
```

默认使用终端交互预览，`Enter` 生成索引；无终端时须显式使用 `--yes`。按 `S` 进入排序，`X` 拿起游戏，上下移动，`Enter` 固定位置；未拿起游戏时 `Enter` 生成；`Esc` 撤销当前移动或返回预览，`Q` 取消。设备包只包含 ROM、`.565` 封面和 `games.idx`，把其内容复制到设备磁盘根目录的 `roms/`。索引记录保存实际 ROM 文件名和相对关系，不携带电脑路径；设备不写回索引或顺序文件。

索引是 `GIDX` v1，小端编码，最多 128 条记录，最大 256 KiB。封面固定为 58×80、RGB565 little-endian、9,280 字节，文件名与 ROM 主干相同，例如 `01_Game.nes` 与 `01_Game.565`。简介为 ASCII 文本，可使用 LF 分段；菜单按实际字体分页，方向键上下翻页，左右切换游戏。无简介或封面时使用内置占位内容。

默认复用 `game-order.json` 中已有顺序；首次按完整显示标题排序，新游戏按标题排序追加。`--order FILE.json` 可指定另一份 PC 顺序文件。未指定 `--output` 时原子更新工作目录中的索引并保留 TXT，指定后仅创建不存在的新部署目录。修改文件名、简介或封面后都应重建索引。

工作目录中的 TXT、`game-order.json` 和预览文件不需要复制到设备。重新生成索引前请先运行 ROM 预检；索引工具只负责元数据和部署包，不替代 ROM 格式检查。

素材使用无头、小端 RGB565 格式，放在设备磁盘内：

| 路径 | 尺寸 | 大小 |
|---|---|---|
| `roms/Game.565` | 58 × 80 | 9280 字节 |
| `assets/nes-background.565` | 320 × 240 | 153600 字节 |

封面应与游戏使用相同文件名主干，如 `01_Game.nes` 和 `01_Game.565`。索引中的封面标志只表示资源存在；设备读取失败时仍显示默认卡带。无自定义背景时依次使用 `/rom/nes-background.565` 或内置背景。

`menu stats` 的 `prepare_us` 是最近一次菜单选择资源准备耗时，缓存命中时通常很小；`draw_us` 是菜单绘制耗时；`frame_us` 包含这两段及等待显示完成的时间，不含输入处理、翻页、启动游戏、启动时首次资源准备或重扫。`max_*` 为启动以来的最大值，单位微秒。连续切换游戏后可用最大值判断资源加载是否导致卡顿；主机测试耗时不能代替设备测量。

封面缓存包含当前游戏及左右相邻游戏，菜单画面提交后利用导航停顿逐张预取。`menu stats` 的 `covers` 行显示选择时的 `hits` / `misses`（包含缺失封面的缓存结果）、`prefetches` 及最近/最大预取耗时。预取时间单独统计，不计入 `frame_us`；它仍占用菜单循环时间，评估响应延迟时应一起查看。缓存不省略简介排版和画面绘制，首次进入、快速连续导航时仍可能现场读取封面。

菜单左右或上下同时按下时，对应轴不导航并清除长按计时；恢复单方向后重新等待 400 ms。切游戏与启动在同一轮出现时只处理切换，启动需松开后重新按下；启动始终使用逻辑选中项，不等待滑动动画结束。缺失或损坏的封面在三槽缓存中记住失败结果，槽位淘汰后再次经过可重试。游戏位置和简介页码的纵坐标固定，不随封面浮动。

封面缓存未命中时先显示新卡片和默认卡带，再读取当前封面，随后才预取左右邻居。读取仍是同步操作，慢读期间动画可能暂停。`menu stats` 的 `current_cover_us` / `max_current_cover_us` 分别记录最近及最大的当前封面加载耗时；与邻居预取一样，这段时间在画面提交后发生，不计入 `frame_us`。

菜单背景覆盖整个 320×240 屏幕，顶部标题、电池与连接图标直接叠加在背景上。背景使用无文件头、逐行 RGB565 little-endian 数据，与封面采用相同编码。更新内置背景可运行 `python tools/update_menu_background.py path/to/background.565`，然后重新编译；该工具只替换背景数组，保留字体数据。
