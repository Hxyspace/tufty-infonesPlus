# 存储与固件升级

## Flash 布局

固件使用以下 16 MiB Flash 布局：

| 区域 | 起始偏移 | 容量 | 内容 |
|---|---:|---:|---|
| 固件 | `0x000000` | 2 MiB | 程序 |
| `/rom` | `0x200000` | 1 MiB | RM1 只读资源 |
| `/system` | `0x300000` | 12 MiB | FAT 卷、游戏和素材 |
| LittleFS 根 `/` | `0xf00000` | 1 MiB | LittleFS v2，包含 `/state/` 配置目录 |

文件访问按 `/rom/`、`/system/`、`/state/` 前缀分发，没有挂载通用根目录 VFS。
`/state/nes.json` 是 LittleFS 卷内的原始路径，并非把卷根映射为 `/state`。
`/rom` 兼容 RM1 ROMFS；菜单和字体位图编入固件，不要求从该分区加载。

刷入前，设备须已具有上述布局及有效文件系统。UF2 不包含文件系统镜像；固件不会自动分区或格式化。仓库不提供空白设备的初始化工具。

## 游戏与 USB 磁盘

游戏位于 `/system/roms/`。运行游戏时 FAT 只读；进入 USB 磁盘模式后固件卸载 FAT，由电脑独占读写。只有 `/system` 对外暴露，`/rom` 和 `/state` 不显示在该磁盘中。

FatFs 支持 512、1024、2048、4096 字节逻辑扇区，底层 Flash 按 4096 字节物理块擦写。磁盘操作完成后应安全弹出，再等待设备自动重启。

## 配置

`/state/nes.json` 保存以下字段：

| 字段 | 默认值 | 用途 |
|---|---|---|
| `controller_id` | `1` | 串口及 TGP BLE 手柄 ID |
| `brightness` | `80` | 屏幕亮度，1–100 |
| `swap_ab` | `false` | A/B 互换 |
| `last_rom` | 空 | 上次选择的游戏文件名 |
| `menu_sleep_seconds` | `300` | 菜单空闲休眠秒数，0 关闭，最大 86400 |
| `serial_enabled` | `true` | 启用 CDC1 手柄会话，不影响 Console 和 USB 磁盘 |
| `ble_enabled` | `true` | 启用 BLE HID / TGP 扫描和连接 |
| `ble_mode` | `"auto"` | `auto` 自动连接兼容设备；`whitelist` 仅连接白名单 |
| `ble_whitelist` | `[]` | 最多四项，每项包含 `mac` 和布尔值 `random` |
| `ble_preferred` | `-1` | 白名单首选项下标，-1 表示不指定 |
| `ble_scan_uuid` | `"0x1812"` | HID 自动扫描时匹配的 16 位服务 UUID；另自动识别 TGP 自定义服务 |

配置通过临时文件和原子重命名写入，其他 State 文件保持原样。State 无效或 NES 配置损坏时使用默认配置，并禁用保存。此文件不包含游戏进度或电池 SRAM。

缺失的配置字段使用默认值。兼容旧字段 `ble_peer` 与 `ble_random`：地址非空时转换为一条首选白名单，并启用白名单模式，下次保存使用新字段。BLE 配对信息仅保存在 RAM，白名单是连接选择策略，不包含绑定密钥。菜单自动休眠及串口休眠会先保存待写配置；长按 Reset 会先重启，使用之前应保存设置。

手柄配置通过 [Console 命令](usage.md#连接设置) 修改，再用 `save` 保存。配置仅从 `/state/nes.json` 加载；USB 磁盘上的旧 `config/gamepad.json` 不生效，可以删除。安卓轮换的随机 MAC 无法靠静态白名单长期识别，建议使用自动模式。

## 升级注意事项

普通固件数据限制在前 2 MiB。UF2 同时保留 Pico SDK 默认的 RP2350-E10 兼容块，其行为取决于芯片修订版本：

- **A3 及以后**：Boot ROM 忽略带标签的兼容块。
- **A2**：Boot ROM 可能写入该块，擦除 State 最后一个扇区；升级前应备份配置。

A2/A3 指芯片修订版本，与 RP2350A/B 封装不同。兼容块地址为 `0x10ffff00`，family 为 `0xe48bff57`，行为与 SDK 默认设置一致。构建后的 UF2 检查分别验证普通固件范围和兼容块。
