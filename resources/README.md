# 固件资源

构建时将三个目录分别打包到设备对应的存储区域，文件路径相对各自目录保留：

默认使用本目录；本地自用资源可复制到 Git 忽略的 `resources-local/`。再运行 `TUFTY_RESOURCES_DIR=resources-local TUFTY_ARTIFACT_DIR=dist-local bash tools/build.sh`。这会整体切换输入根目录，不与默认目录合并。首次复制和索引生成步骤见[本地资源构建](../docs/building.md#本地游戏和资源)。

| 源目录 | 设备位置 | 格式 / 容量 |
|---|---|---|
| `resources/rom/` | `/rom/` | RM1 / 1 MiB |
| `resources/system/` | `/system/` | FAT12，4096 字节扇区 / 12 MiB |
| `resources/state/` | LittleFS 根 `/` | LittleFS v2 / 1 MiB |

例如 `resources/state/state/nes.json` 对应设备 `/state/nes.json`，不要省略第二层 `state`。默认保留空 `/state` 目录，配置使用程序默认值。

默认 RM1 包含月夜背景 `nes-background.565`（320×240，RGB565 little-endian，无文件头）。替换此文件后重新构建带 ROM 的镜像即可更新。字体仍编入程序。FAT 的 `roms/` 目录包含 Chase、LAN Master、TobuNES、Cargo Shift 四款游戏、各自封面和 `games.idx`；TXT 为主机侧简介源，固件读取索引中的简介。刷入 `with-filesystem` 后可直接从菜单启动。TobuNES 的 NES 2.0 多区域声明默认按 NTSC 运行，原始 ROM 不修改。

`resources/system/roms/` 是随镜像发布的资源目录。整个 `resources/` 在 Git 中默认忽略，新增资源需使用 `git add -f <具体文件路径>` 明确加入；已有资源仍被跟踪，修改和删除会正常显示，忽略规则不会保护它们免于提交。不要强制添加整个目录，以免带入无关文件。根目录 `roms/` 则用于本地预检和索引准备，除说明文件外均被 Git 忽略。Git 不记录空目录，空的 `state/state/` 使用 `.gitkeep` 占位。忽略规则不影响本地构建；资源目录中的文件仍会进入完整镜像。

隐藏文件/目录（包括 `.gitkeep`）不进入镜像；禁止符号链接。FAT 文件名大小写冲突会报错。文件系统容量不足时构建失败，不截断文件。源目录为空时仍生成有效的文件系统。

运行 `bash tools/build.sh` 同时生成三种 UF2，详见 [构建指南](../docs/building.md)。`with-filesystem` 镜像会**替换设备全部资源、游戏和配置分区**；日常更新使用 `flash`，更新只读素材使用 `with-romfs`。资源目录只是构建输入，不会自动与设备文件合并。

带资源的 UF2 使用 sparse 打包，省略文件系统未使用的擦除扇区，分区容量保持不变；完整镜像仍会重建游戏目录及配置。省略的空闲扇区可能保留旧数据，不用于安全擦除。
