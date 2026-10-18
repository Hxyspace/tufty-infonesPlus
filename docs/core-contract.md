# 核心与宿主契约

`nes_core` 只包含模拟机器，`nes_cartridge` 提供只读字节流检查和单实例加载。
应用调用 `nes::infones_adapter::load()`，成功后运行 `InfoNES_Cycle()`，
退出时调用 `unload()`。不要用旧 pico-nes 的 `InfoNES_Main(true)` 接口。

ROM 缓冲由宿主持有，从加载到卸载地址保持稳定，不得并发修改。
格式或板型检查失败时保留当前游戏；分配失败时清理资源，返回静态错误字符串。
宿主不得在显示、音频或 `service()` 回调内重入核心、加载或卸载。
所有模拟状态只归模拟线程；另一核仅消费已经发布的显示缓冲。

初期公共加载入口接纳 Mapper 0、1、2、3、4、7、48、66 的常规几何布局：
PRG/CHR 容量为二的幂，PRG 最多 512 KiB，CHR 最多 256 KiB，
并施加 NROM/CNROM/UxROM/AxROM 等板型的额外约束。
其他 Mapper 源码保留，但须增加板型尺寸、寄存器写入、IRQ、持久化回归后，
再扩大入口支持范围。Mapper 表有编号不等于板型通过验证。

NES 2.0 保留原头，支持标准 NES、明确 NTSC/PAL/Dendy、submapper 0、
旧核心可表达的 ROM 大小和受限 RAM：PRG RAM 总量不超过 8 KiB，
不混合易失与非易失 PRG RAM，CHR RAM 为 8 KiB，无 CHR NVRAM 或 ROM/RAM 混合。
更大 bank 数、指数容量、多区域选择、额外硬件及未验证 submapper 明确拒绝。
未知扩展 Mapper 不降级到低八位。iNES 的歧义脏头也不自动猜测修复。
这是格式与内存布局的受限支持，不宣称所有对应游戏均已验收。

`InfoNES_Port.h` 提供分配/释放、大内存能力、精灵限制选项和每扫描线服务。
分配必须返回空指针表示失败；释放接受空指针。
核心锁存包括延迟 Flash 缓冲在内的分配失败；运行到下一扫描线安全边界退出。
应用检查 `infones::memory::failed`，报告错误并卸载。底层 `InfoNES_Reset()`
失败后的资源由调用方 `InfoNES_Fin()` 清理；公开加载入口自动完成清理。
`InfoNES_Fin()` 可重复调用；宿主释放 ROM、关闭声音回调也须支持重复调用。

像素行是 256 个 `WORD`，bit 15 是背景透明标志，**不是 RGB565**。
宿主提供 bit 15 不占用的 `NesPalette[64]` 色值，在 `PostDrawLine()` 转换/复制。
全部 240 行都会输出；BG 与 Sprite 左侧遮罩独立。
行缓冲至少 256 个像素，必须存活到该行绘制结束；异步显示须另有已发布的完整帧。
灰度、强调和帧内调色变化需在呈现层契约中明确，不可假装六位色号保存了全部状态。

声音输出仍是五路基础波形和可选扩展波形。APU/DMC 始终推进，
容量、静音只改变交付的样本，回调必须同步消费/复制，不能保留借用指针。
无音频设备可报告零容量并提供空消费回调。SoundOpen 返回 0 表示成功。

`InfoNES_LoadFrame()` 的返回值目前不控制退出；退出由输入回调的
`PAD_SYS_QUIT` 决定。按钮按照核心 NES bit 顺序传入。
主机调用服务不得依赖呈现帧率；显示 pacing 不能改 CPU/PPU/APU 的逻辑时钟。

测试使用合成 ROM 链接真实 CPU/PPU/APU/Mapper，覆盖顺序取数、JSR/RTS、
完整行输出、左右裁剪、Mapper48、DMC 背压/静音、截断文件与失败清理。
初期加载矩阵仍需设备和真实 ROM 验证，不能由合成测试替代。
