# 固件资源

构建时将三个目录分别打包到设备对应的存储区域，文件路径相对各自目录保留：

| 源目录 | 设备位置 | 格式 / 容量 |
|---|---|---|
| `resources/rom/` | `/rom/` | RM1 / 1 MiB |
| `resources/system/` | `/system/` | FAT12，4096 字节扇区 / 12 MiB |
| `resources/state/` | LittleFS 根 `/` | LittleFS v2 / 1 MiB |

例如 `resources/state/state/nes.json` 对应设备 `/state/nes.json`，不要省略第二层 `state`。默认保留空 `/state` 目录，配置使用程序默认值。

默认 RM1 包含月夜背景 `nes-background.565`（320×240，RGB565 little-endian，无文件头）。替换此文件后重新构建带 ROM 的镜像即可更新。字体仍编入程序。FAT 默认只有空的 `roms/` 目录，不附带游戏 ROM；可以将自己准备的 `games.idx`、ROM 和封面放入此目录再构建完整镜像。

Git 不记录空目录，因此 `system/roms/` 和 `state/state/` 使用 `.gitkeep` 占位。`resources/system/roms/` 中除占位文件外的内容均被 Git 忽略，包括游戏、封面和索引；忽略规则不影响本地构建，这些资源仍会进入完整镜像。发布自定义构建前请检查其中包含的游戏内容。

隐藏文件/目录（包括 `.gitkeep`）不进入镜像；禁止符号链接。FAT 文件名大小写冲突会报错。文件系统容量不足时构建失败，不截断文件。源目录为空时仍生成有效的文件系统。

运行 `bash tools/build.sh` 同时生成三种 UF2，详见 [构建指南](../docs/building.md)。`with-filesystem` 镜像会**替换设备全部资源、游戏和配置分区**；日常更新使用 `flash`，更新只读素材使用 `with-romfs`。资源目录只是构建输入，不会自动与设备文件合并。

带资源的 UF2 使用 sparse 打包，省略文件系统未使用的擦除扇区，分区容量保持不变；完整镜像仍会重建游戏目录及配置。省略的空闲扇区可能保留旧数据，不用于安全擦除。
