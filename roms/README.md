# 本地 ROM

此目录用于用户自己的 ROM、封面、简介和菜单资源，不随仓库或固件分发。
`tools/prepare_roms.py` 默认检查这里，只有显式 `--fix` 才改文件并备份。
检查工具仅作预检，实际可接受的板型尺寸和 RAM 由固件加载器判断。
在项目根执行 `python3 tools/build_game_index.py` 预览，Enter 生成、S 排序，排序时 X 拿起、上下移动、Enter 固定；未拿起时 Enter 生成，Esc 撤销当前移动或返回预览，Q 取消。`--input roms` 指定工作目录，`--yes` 跳过交互；无终端时必须使用 `--yes`。

默认原子更新此目录的 `games.idx` 并保留 TXT，顺序复用 `game-order.json`；首次按完整标题排序，新游戏按标题排序追加，`--order FILE.json` 可指定另一份 PC 顺序文件。改名或修改简介/封面后需要重建索引。

`python3 tools/build_game_index.py --input roms --output deploy/roms --yes` 创建必须不存在的新设备包目录，仅含 ROM、同名 58×80 `.565` 封面和 `games.idx`。进入 USB 磁盘模式后，把包内文件复制到设备盘根目录的 `roms/`，无需另外建立 `system/`，完成后安全弹出。索引文件名保留原始大小写、相对索引目录解析；TXT 和顺序 JSON 留在电脑，无需部署。交互排序和素材格式见[使用指南](../docs/usage.md#游戏索引封面和背景)。
文件系统布局和素材格式见[使用指南](../docs/usage.md)。
