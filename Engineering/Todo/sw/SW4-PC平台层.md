# SW4 PC 平台层

状态：**代码完成，PC 侧判据全部通过（2026-09-21）；两条还没收口**：
板上那三个查询没有上板跑过（归 [SW9](SW9-板上移植.md) 一起验），
第 9 条关窗退出与 F1~F3 切窗口要人动手点一次（见第 4 节「还欠什么」）。
前置 SW1 已于 2026-09-19 验收：
`sw/bsp/include/board.h` 的接口与 `sw/bsp/host/board.c` 的最小实现（开窗、按 4096 行跨上传纹理、
60Hz 节拍、按键位图、周期折算与注错）已经在库里。本条是把这份最小实现补成**与板上逐条同语义**
的完整平台层。阻塞 SW6、SW7 在 PC 上的演示与试玩。

开工前先做了一轮前期研究：读既有代码与扫描输出的 RTL、在 WSL 里实测 SDL2 的行为、
用探针对场节拍模型取样，落到 1.1 节的十项取舍（D1~D10）与第 3 节的量化判据，
过程与原始数据在第 4 节。研究推翻了原文四处（2.2 的两处、2.5 的动机、2.4 的文件读写归属），
另在 2.7 节列清了要改的既有代码。

## 1 要解决什么

本条补全 `sw/bsp/host/`，即 PC 上的「板子」。PC 版的作用是在没有板子的情况下开发游戏、
调画面、出第一版计算统计（设计文档 4.4、4.5 节）。

SW1 已经让窗口亮起来，缺的不是像素，是**语义**。扫描输出于 2026-09-21 验收
（[display 第 01 章](../../TechReports/display/01-从DDR3帧缓冲到HDMI的扫描输出.md)），
`board_present` 与 `board_wait_frame` 在板上的行为已经定死在硬件寄存器上，
PC 侧必须照这个既成事实实现，而不是照设计文档 3.7 节那段面向引擎的描述。
两边差一点，PC 上正确的帧切换逻辑到板上就会错，SW9 的逐帧比对也就失去意义。

**这一条的风险不在功能在模型。** PC 上能跑通的最小实现 SW1 已经有了，
难的是让「场数、切换数、掉帧、每次 present 返回的时刻」这些量在两边是同一个量：
差一场，掉的帧算错一场；差一个调用，帧率差一倍。所以本轮先把模型用探针取样钉死，
再动代码。

### 1.1 开工前要确认的十项取舍（D1~D10）

十项都在 2026-09-21 的前期研究里定案，依据是第 4 节的实测数据。
工作量为零的前置研究换来的是：不用在写完之后靠「PC 上看着对」去猜板上对不对。

**D1 场节拍怎么建模。** 板上 `SWAP_REQ` 是一个粘滞请求位，写 1 之后**在下一个场起始锁存并清零**
（`rtl/display/display_fetch.v` 第 344~357 行），所以无论请求写得比场起始早多少，
锁存都发生在紧接的那一个场起始；请求不会被漏掉，也不会攒账。
建议：PC 的场计数定义成自由运行的 `(now − base) / step`（`step = freq / 60`），
present 进入时记下场号 `f0`，**等到场号大于 `f0` 再返回**，返回时切换数加一。

不采用「每次把网格推进一个 step 再等」（SW1 现在的 `board_wait_frame` 就是这么写的）：
它只保证平均节拍，表达不了「锁存在下一个场起始」，落后一帧之后会连续几帧立即返回。
实测（4.2 节）：最小间隔 1112 微秒，600 帧里场数增量 599 而切换增量 600，
掉帧减法回绕成 2^64−1。

**结论（2026-09-21 定案）：自由运行场计数 + 等下一个场边界。**
`board_wait_frame` 与 `board_present` 共用这一个原语。

**D2 掉帧数归谁、怎么算。** 两个平台都真实存在的量是场数与切换数，
在 `board.h` 补一对只读查询（2.2 节），掉帧数由应用做减法：`Δ场 − Δ切换`。
PC 上 `board_frame_count` 由 60Hz 网格给、`board_swap_count` 由成功 present 的次数给；
板上分别读 `FRAME_COUNT` 与 `SWAP_COUNT`。

**结论：按上述两种来源实现同一个减法，`FRAME_DROP` 不进平台层。** 同 2.2 节。

**D3 无窗口模式怎么做。** 三个候选：不初始化视频子系统、显式 dummy 驱动、靠 SDL 自己落到
offscreen。实测（4.4 节）：`SDL_VIDEODRIVER=dummy` 下 init、开窗、建纹理、上传都照常，
只有 `SDL_CreateRenderer(ACCELERATED)` 失败并落到已有的 SOFTWARE 分支，
`RenderPresent` 是空操作，节拍数字与窗口组逐条一致；不初始化视频子系统也跑得通，
但要在窗口、渲染器、纹理三处加空指针判断，即「有窗/无窗」不再是同一条路径。

**结论：headless = `SDL_VIDEODRIVER` 置成 `dummy`**（在 shell 里给，程序里不写代码，见 D5 的补充段）。
init 之后没有任何分支，判据「无窗与有窗 CRC 序列逐帧相同」才是同一个执行路径的对照。

**D4 缩放与像素采样。** `SDL_HINT_RENDER_SCALE_QUALITY` 的默认值是**未设**
（`SDL_GetHint` 返回 NULL），所以不能依赖 SDL 的默认；显式设 `"0"`（最近邻）。
实测：`"0"` 下 8×8 棋盘放大 80 倍是硬边（x=79 黑、x=80 白），`"1"` 下边界两侧都是 0x7f/0x80 的插值。
渲染器**不能带 `SDL_RENDERER_PRESENTVSYNC`**：PC 的场节拍由性能计数器的网格给，
交给显示器刷新率之后，高刷屏上量出来的帧率不是 60。

**结论：显式设 hint "0"，不带 PRESENTVSYNC，缩放只改 `SDL_RenderCopy` 的目标矩形。**

**D5 运行模式怎么选。** `sdk/software/standalone/common/start.S:81` 是 `call main`，
板上没有 argc/argv，参数寄存器是未定义的；现有十个应用全部写 `int main(void)` 或 `void main(void)`。
（`lib/test/test_main.c` 用了 argv，但 `lib/test/` 只跑主机、不进应用的库列表。）
环境变量在板上同样没有来源。

**结论：共享的应用不用 argv 也不读环境变量。** 测试应用一次跑完全部场景、不加开关；
互动开发（SW6、SW7）本来就不需要开关：游戏应用开窗直接跑。

**这一条管的是「应用自己的开关」，不包括 `SDL_VIDEODRIVER`。** 后者是 SDL 库自己读的
宿主配置，不是应用的选项：`bsp/ti60` 根本不链接 SDL，不存在「板上没有来源」的问题，
这与 `sdl2-config --libs` 只出现在 `mk/host.mk` 里是同一件事。所以 headless
**不需要编译宏，也不需要程序里写一行**，在 shell 里给就行：

```
SDL_VIDEODRIVER=dummy ./build_host/platformTest
```

4.4 节的三组数据本来就是这么跑出来的。用编译宏反而会坏掉 D3 的理由：第 8 条判据
要的是「同一条执行路径」，编译宏让有窗与无窗成了**两个不同的二进制**，
比的就不再是同一份代码。用环境变量时两次跑的是同一个 ELF，判据才立得住。

**D6 录像格式。** 定长、无对齐洞、多字节量显式小端，一步能转成 C 数组，LP64 与 ILP32 解出同一串按键。
建议格式：

```text
偏移  长度  内容
0     4     魔数 "G2DR"（小端读作 0x52443247）
4     2     版本号，当前 1
6     2     标志位，当前 0（保留）
8     4     随机种子
12    4     帧数 N
16    4*N   每帧一个 32 位按键位图，位分配同 board.h 的 BOARD_KEY_*
```

总长 `16 + 4N`，全是 4 字节对齐的量，两边结构体都不会插洞；解析仍逐字节按小端读写，
不把文件缓冲强制转成结构体（对齐、别名、端序三件事都不用赌）。

**结论：按上表定 ABI 1，放进 `sw/lib/core/replay.h`。** 与 spec 第 02 章的向量交换格式同一套写法。

**D7 录像数据怎么进程序。** 板上没有文件系统，SW9 要把录像编进程序；PC 上若用文件，
就要在两边共编的应用里出现只有一边有的服务。原文把文件读写归 `bsp/host`，现在撤掉：

| 谁 | 管什么 |
| --- | --- |
| `sw/lib/core/replay` | 格式、一段字节缓冲、打包与解包、逐帧取键与逐帧追加。纯 C，两个平台同一份 |
| `bsp/host` | 没有文件接口，不认「帧」，也不认录像 |
| 应用 | 每帧决定按键从哪来：实时调 `board_keys()` 并喂给 replay 记下；回放只从 replay 取，**不调 `board_keys()`** |
| 录制出来是什么 | 应用把打包好的字节按固定文本行经 `board_puts` 打出（板上就是串口日志），`Main/tools/check/replay2c.py` 把这串文本或 `.g2dr` 二进制转成 C 数组 |

**结论：回放一律读编译进程序的 C 数组，录制结果就是日志。** 代价：录一段要过一次脚本；
换来的是 PC 与板上同一份数据、同一个解析器，`board.h` 不新增只有一边有的服务。

**D8 CRC 口径与落点。** 沿用 SW3，不另立一套：逐行取前 `width × 4` 字节、共 `height` 行，
**跳过行跨的间隙**，用 `lib/core/crc32.h` 的流式接口。参照实现是
`sw/lib/test/ref_crc.h` 的 `ref_crc32_strided()`；`sw/lib/test/test_gfx.c` 里的 `frame_crc()`
是同一口径的第二份，不回退成调用产品代码。

行跨 4096 而一行只有 640×4 = 2560 字节，每行有 1536 字节空隙；把空隙算进去，
PC 上 `calloc` 的零与板上 DDR 的上电残值必然不同，SW9 的逐帧比对会全线报错，而错因与渲染无关。

**结论：应用侧写逐行 update 的循环，与 golden.txt、SW9 共用同一口径。**

**D9 分层落点。** 三方分工见 D7 的表。判断标准用 SW1 的：`board.h` 只放「与硬件有关、
两边都能实现的服务」。文件系统与命令行都不是这种服务，所以两者都不进 `board.h`；
录像的格式与逐帧逻辑是纯算法，进 `lib/core`。

**结论：`sw/lib/core/replay.{h,c}` 是录像的唯一真源。**

**D10 哪些判据在 PC 上根本不成立。** 原文第 3 节「掉帧」一项里的「前缓冲在这一场内没有被改写」
与下一句的「画面不撕裂」是扫描输出与引擎的性质，PC 上帧缓冲只有应用一个写者，拿去判是自证。
PC 侧能判的是**调用序列与计数**（每次 present 恰好等掉一场、场数与切换数的增量关系、
掉帧减法），撕裂与改写留给 SW9 与引擎。**结论：第 3 节按这个边界重写，该句已删。**

## 2 怎么切分

### 2.1 显示

- [x] SDL2 开 640×480 窗口，像素格式 `SDL_PIXELFORMAT_XRGB8888`，小端下字节序与
  spec 第 01 章第 7 条一致；两块缓冲取自 `board_region` 的帧缓冲 0、1，
  行跨与板上相同取 4096 字节，上传纹理时按 `FB_STRIDE_BYTES` 的 pitch 交给
  `SDL_UpdateTexture`，不另拷贝。SW1 已实现，见 `sw/bsp/host/board.c`。
- [ ] 整数倍放大：F1~F3 切 1×、2×、3×，缩放用最近邻，**显式** `SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0")`
  （默认值是未设，见 D4）。
  **缩放键不进 `board_keys` 的位图**：窗口大小是宿主的事，进了位图就会被录进录像、
  改变回放的按键序列，破坏约束 3。`bsp/host` 在自己的事件循环里吃掉这三个键。
  帧缓冲尺寸恒为 640×480，变的只有窗口与 `SDL_RenderCopy` 的目标矩形。
- [ ] 渲染器用 `SDL_RENDERER_ACCELERATED`，失败落 `SDL_RENDERER_SOFTWARE`（SW1 已有），
  **两个都不带 `SDL_RENDERER_PRESENTVSYNC`**。三种窗口尺寸 640×480 / 1280×960 / 1920×1440
  在本机（display 0 = 2560×1600）都建得出来，实测见 4.3 节。

### 2.2 帧节拍与双缓冲

板上这两个接口的契约（`bsp/include/board.h`，扫描输出验收后定稿）：

| 接口 | 板上行为 | PC 上要做成什么 |
| --- | --- | --- |
| `board_present(i)` | 写 `BACK_BASE` = 帧缓冲 i、`SWAP_REQ` = 1，**轮询 `SWAP_COUNT` 递增之后才返回**；切换在场起始生效，所以不撕裂 | 上传纹理、刷新窗口，然后**阻塞到下一个 60Hz 场边界才返回** |
| `board_wait_frame()` | 轮询 `FRAME_COUNT` 递增，与 `EN` 无关，没 present 过也能用 | 等到下一个 60Hz 场边界 |

**每帧的标准序列是「画后缓冲 → `board_present(back)`」，不再另调 `board_wait_frame()`。**
present 自己已经等掉一场，两个都调会掉到 30 fps。`board_wait_frame` 留给不出画面的场合：
对齐节拍、只读计数。`sw/apps/test/displayTest` 的动画段就是这么写的，板上实测 59.9 fps。

PC 侧现在的 `board_present` 上传完纹理立刻返回，节拍另由 `board_wait_frame` 提供。
**这是本条要改的第一处**：把等待搬进 `board_present`。

**模型的定稿形态（D1）。** 场是一根自由运行的时间轴，`base` 在 `board_init` 取一次：

```text
step       = freq / 60                     /* freq = SDL_GetPerformanceFrequency */
场号(now)  = (now − base) / step           /* board_frame_count 返回它，32 位回绕 */
等一场     = 等到「场号(now) > 进入时的场号」
board_present(i)  = 上传纹理、刷新窗口 → 记 f0 = 场号(now) → 等一场 → swap_count++
board_wait_frame() = 记 f0 = 场号(now) → 等一场
```

等待本身用「超过 1 毫秒就 `SDL_Delay(整毫秒 − 1)`，最后一毫秒自旋」的混合写法：
本机 `SDL_Delay(1)` 实测 1058~1149 微秒，亚毫秒的睡眠粒度不存在。
两个平台真实存在的量仍然是场数与切换数，所以在 `board.h` 补一对只读查询，两个 bsp 各自实现：

```c
/* 都按 32 位回绕，与 board_cycles 同一约定：取两次读数相减。 */
uint32_t board_frame_count(void);   /* 板上读 FRAME_COUNT；PC 上是 60Hz 节拍计数 */
uint32_t board_swap_count(void);    /* 板上读 SWAP_COUNT；PC 上是成功 present 的次数 */
```

一帧超时的表现是场数涨 2 而切换数只涨 1，所以**掉帧数 = 两者增量之差**，由应用自己算。
SW8 的统计埋点用同一个减法，板上与 PC 上是同一份代码。引擎出来之后 `PERF_FRAME_DROP`
是引擎自己的口径，与这条并存，不互相替代。

**原来说「掉帧计数归应用」时没有说清 PC 侧的两个来源。** 现在写明：
`board_frame_count` 在任何时刻都可以读（与有没有 present 无关，板上 `FRAME_COUNT` 由像素域
时序发生器计数，PC 上由时间轴给），`board_swap_count` 只在 present 返回之后才增加。
两者的差就是这一段时间里错过的场数。

**已知的建模误差（记在案，不再扩大）：** 板上 present 在「场起始 + 六步清空」之后返回
（扫描输出 2.1 节 D7 的排空、复位与等待那六步），实测那一串最多几十个 `core_clk`（display 第 01 章 3.4 节），
所以板上返回时刻比 PC 晚约 2 微秒。该误差只影响「一帧刚好压在 16.7 ms 上」这一条边界，
判据里的注错帧用 20 ms，离边界很远。

**两个增量之差要在 32 位上算完再用。** 接口契约是 32 位回绕，`Δ场 − Δ切换` 必须
先在 `uint32_t` 上减完、判完，再往上累加：

```c
uint32_t d = (uint32_t)(f1 - f0) - (uint32_t)(s1 - s0);   /* 掉帧数，32 位 */
total_drop += d;                                           /* 累加放更宽的类型才安全 */
```

先提升再减会把一次回绕放大成天文数字：4.2 节旧算法那一行的 `drop=2^64-1` 就是这么来的
（探针在 64 位上做的减法；按本节契约在 32 位上算应当是 2^32−1）。
SW8 要按秒累加掉帧数，踩到一次就整列作废。

#### 关窗口之后怎么退出

`board.c` 现在把 `SDL_QUIT` 记在静态变量 `s_quit` 里，只用来让 `board_wait_frame` 提前返回，
**应用无从知道**。SW1 的程序跑固定帧数就结束，所以没暴露；SW6、SW7 是「直到退出为止」的循环，
照现在的实现点窗口关闭按钮不会退出，只会空转。present 换成阻塞等场边界之后更明显：
关了窗口，每一帧照样各等掉一场。补一个接口：

```c
/* 宿主要求结束（PC：SDL_QUIT）。板上恒为 0。 */
int board_should_quit(void);
```

置位之后 `board_present` 与 `board_wait_frame` 都立即返回，不再等场边界，
否则退出前还要多挂一场。退出不是游戏输入，**不进按键位图**：
进了位图就会被录进录像，回放时凭空多出一个按键，破坏约束 3。

### 2.3 输入

- [x] 按键位图与键位已由 SW1 定稿实现：方向键与 WASD 移动，空格炸弹，左 Shift 低速，
  Tab 切软硬件（PC 上只有软件路径，记日志），数字键 1~3 切三种演示模式。
  折成每帧一个位图，与板上 KEY0~3 加串口的来源无关。
- [ ] 输入来源可切换：实时键盘与录像回放两种。**切换不在 `bsp` 里做**，见 2.4。

### 2.4 录制与回放

分层的落点见 D7，这里是实现形态：

- 录像格式见 D6，读写都在 `sw/lib/core/replay`，`bsp` 一次都不碰字节。
- 录制：每帧的按键位图追加到缓冲；退出时把缓冲按 `hex <偏移> <8 个字>` 的文本行经
  `board_puts` 打出。PC 与板上都是这一段代码，板上打出来就是串口日志，PC 上重定向成文件。
- 回放：从编译进程序的 C 数组读按键，**不调 `board_keys()`**；
  每帧打印帧号与后缓冲 CRC32，供两条序列比对。
- `Main/tools/check/replay2c.py`：把上面那串文本或 `.g2dr` 二进制转成 C 数组，
  并能对已入库的数组做一致性回读（`--check`），与向量工具同级、同一处。
- 同一份录像在 PC 上回放两次，CRC 序列必须完全相同，这是确定性（总览约束 3）的判据。
- 注错：让应用读一次 `board_cycles()` 并参与画面计算，两次回放的 CRC 序列必须出现不同。

**CRC 的口径沿用 SW3，不另立一套**：逐行取前 `width × 4` 字节、共 `height` 行，
**跳过行跨的间隙**，流式接口用 `lib/core/crc32.h`，参照实现见 `sw/lib/test/ref_crc.h`
的 `ref_crc32_strided()`。

### 2.5 无窗口模式

无窗口模式要求在**没有显示的环境**里跑完，做法见 D3：把 `SDL_VIDEODRIVER` 置成 `dummy`，
`SDL_Init` 之后一行分支都不加。**置法是在 shell 里给环境变量，程序里不写代码、不加编译宏**
（D5 的补充段）：`SDL_VIDEODRIVER=dummy ./build_host/platformTest`，
有窗与无窗跑的是同一个二进制。

**节拍与阻塞语义照常走**，不因为没有窗口就改成立即返回：有窗与无窗必须是同一条执行路径，
否则 headless 跑出来的 CRC 序列证明不了有窗那一条。

**订正原文的动机。** 原文写「现在的 `board_init` 无条件 `SDL_Init(SDL_INIT_VIDEO)` 并开窗，
这种环境下会直接退出」。实测不成立：把 `DISPLAY`、`WAYLAND_DISPLAY`、`XDG_RUNTIME_DIR`
都去掉之后，SDL 2.32.10 自己落到 `offscreen` 驱动，`SDL_Init(VIDEO)` 与
`SDL_CreateRenderer(ACCELERATED)` 都成功。真实的问题是**落到哪个后端由环境决定、不受控**
（本机有 WSLg 时是 x11，去掉显示相关变量后是 offscreen，只有显式设定才是 dummy），
三个后端的渲染器实现与阻塞行为各不相同，那样的 headless 与有窗不是同一个执行路径，
第 8 条判据也就不成立。所以 headless 要显式指定驱动。

### 2.6 计时与日志

- [x] 计时用 `SDL_GetPerformanceCounter` 折成与板上相同的「周期」单位，32 位回绕，
  只作显示、不作性能结论（总览约束 5）。SW1 已实现，含越界注错 `INJECT_HOST_CYCLES_NAIVE`。
- [x] 日志走 `stdout`，与板上 UART 逐字节相同。
- [ ] 统计 CSV 的字段与汇总脚本两边共用一份，随 SW8 落地。

本条**不含**：音频、手柄、窗口化以外的全屏模式。

### 2.7 受影响的既有代码

逐条列出，避免下一轮漏改：

| 文件 | 改动 | 为什么要跟着改 |
| --- | --- | --- |
| `sw/bsp/include/board.h` | 补 `board_frame_count` / `board_swap_count` / `board_should_quit` 的声明与契约 | 2.2 节，三个都是两个平台都要实现的服务 |
| `sw/bsp/host/board.c` | `board_present` 改阻塞、`board_wait_frame` 换成本条的原语、加三个查询、`s_quit` 置位后两个等待都立即返回、缩放键与窗口尺寸 | 本条主体。headless 不改代码，见 2.5 |
| `sw/bsp/ti60/board.c` | 加两个计数查询的实现（读 `DISP_FRAME_COUNT` / `DISP_SWAP_COUNT`），`board_should_quit` 恒返回 0 | 同一个接口的板上实现 |
| `sw/apps/game/gfx2dGame/src/main.c` | 去掉多出来的一次 `board_wait_frame()` | present 变阻塞之后再调它，帧率掉到 30 fps |
| `sw/lib/core/replay.{h,c}` | 新增 | D6、D7 |
| `sw/lib/test/` | 加录像格式的解析用例（含 `-m32` 与 `-m64` 两边比对），附一份小的 `.g2dr` 固件 | 第 7 条判据 |
| `Main/tools/check/replay2c.py` | 新增 | D7 |
| `Main/tools/check/check_sw4_pc.sh` | 新增：跑应用、对 `golden.txt`、比对有窗与无窗两条序列 | 第 1、8 条判据要跑多次再比，应用自己判不了 |
| `Main/tools/check/check_sw4_scale.c` | 新增主机侧探针：读回放大后的边界像素判最近邻（做法见 4.3 节） | 第 2 条判据的最近邻一半，纯主机、不依赖板子 |
| `Main/sw/README.md` | 补 SW4 的期望输出与运行方式；注明 `gfx2dGame` 的板上日志采于扫描输出之前 | 登记当前状态 |

`gfx2dGame` 的板上日志（`logs/board_normal.log`）是 2026-09-19 采的，那份比特流里没有扫描输出，
`board_present` 与 `board_wait_frame` 当时都是空操作，所以 60 个 CRC 仍然有效；
要改的只是应用里的调用序列，`sw/README.md` 里写明这一点即可。

### 2.8 开工顺序

按依赖排，每一步的产物都能单独验证：

1. `lib/core/replay.{h,c}` 与它的 m32/m64 用例。**先做这个**：纯算法，不碰平台，
   做完第 7 条判据就落地了。
2. `Main/tools/check/replay2c.py`，配一份小固件，把第 7 条判据接上（脚本进判据路径，
   不做一次性生成器）。
3. `board.h` 的两个查询 + `bsp/ti60` 的实现。接口先齐，PC 侧才好写。
4. `bsp/host/board.c` 的场节拍模型与两个查询。做完先量第 3、4 条判据（600 帧节拍、注错掉帧）。
5. 缩放键、窗口尺寸、最近邻 hint，外加 4.3 节那支边界像素探针（沉淀成
   `Main/tools/check/` 下的主机侧探针，与 `check_round255.c` 同级）：第 2 条判据。
6. headless：第 8 条判据。这一步不改代码，是把第 5 步之前做出来的同一个二进制
   换上 `SDL_VIDEODRIVER=dummy` 再跑一次，两条 CRC 序列互比。
7. 测试应用 `sw/apps/test/platformTest`：整帧十一用例、600 帧节拍、注错掉帧、录制与两次回放、
   关窗退出（第 9 条）。

   十一用例接上来不用改 `lib/gfx`：`gfx_case_build(idx, t, m, fb, tex, cmds, &n2)` 的
   `fb` 与 `tex` 本来就是参数，`gfx_mem` 是 `{dev_base, host, bytes}` 的窗口，
   两个平台各自填。**把 fb 与 tex 放进同一个分区**，窗口就是那个分区，
   两边都不用把整块 DDR 的基址暴露出来：帧缓冲 0 分区有 4MB，画面只占前 480×4096 = 1.875MB，
   纹理 64×64、`stride_log2 = 8` 只要 16KB，放在分区内偏移 `0x20_0000` 处，两者不重叠。
   这样画出来的就是 `REGION_FB0` 本身，可以直接 present 出来看。
   CRC 仍按 D8 逐行算，与 `golden.txt` 对。
8. `check_sw4_pc.sh` 把第 1、8 条判据串起来。
9. `gfx2dGame` 的调用序列与 `sw/README.md`、`doc/开发环境.md` 的登记。

第 1、2 步与第 3~6 步互不依赖，可以并行。

## 3 判据

九条判据都在 PC 一侧：第 7 条在主机侧单元测试里（不经 SDL），第 2 条的最近邻一半在主机侧探针里，
其余跑真实应用。第 1~8 条的编号与 2.8 节、D3、D4 里的引用一致，第 9 条（退出）是后加的。**原文「掉帧」里「前缓冲在这一场内没有被改写」半句已删**，理由见 D10。

| 项 | 判据 |
| --- | --- |
| 显示 | 跑 `sw/lib/gfx` 的十一个整帧用例（SW3 已入库），每帧 CRC 命中 `sw/lib/test/golden.txt`；同时目测窗口里没错位，并对首尾两行逐像素比对，判行跨 4096 有没有接错 |
| 缩放 | 1×、2×、3× 三种窗口下同一帧的 CRC 逐位相同；最近邻由主机侧探针读回边界像素判（做法见 4.3 节，沉淀成 `tools/check/` 下的探针）；**缩放键不进位图**照 `keyTest` 的做法要人按：应用把每帧位图打出来，按 F1~F3 的窗口期内这一列必须一位都不变 |
| 帧节拍 | 连续 600 帧空渲染，**每一帧的场数增量都是 1**（这一条是结构性的：「立即返回」会让某帧的增量变成 0、「等两场」会变成 2，两种坏法都被它挡住），`board_frame_count` 与 `board_swap_count` 的增量都是 600；相邻两次 `board_present` 返回的挂钟间隔只作数量级检查，落在 10~25 毫秒（实测空载 16.5~16.9，同时跑一次编译时最坏 15.3~18.0，尾巴归宿主调度，见 4.5 节） |
| 掉帧 | 人为让某一帧渲染耗时 20 ms，该帧场数增量为 2 而切换数增量为 1，应用算出的掉帧数加一 |
| 输入 | 所有键位在日志里打印位图，逐键核对；回放段不读键盘，所以回放那次位图列必须与录像里的按键序列逐帧相同 |
| 确定性 | 一段 30 秒的录像（1800 帧）回放两次，两次的 1800 行 CRC 逐行相同；注错：让应用读一次 `board_cycles()` 并参与画面计算，两次的序列必须出现不同 |
| 录像格式 | 同一份录像经 `replay2c.py` 变成 C 数组后，`-m32` 与 `-m64` 两份主机测试解析出的按键序列逐帧相同，且与直接解析 `.g2dr` 的结果相同 |
| 无窗口回放 | `SDL_VIDEODRIVER=dummy` 且去掉显示相关环境变量时跑完，退出码 0，每一行 CRC 与有窗模式逐帧相同；两次跑的是同一个二进制 |
| 退出 | 窗口关闭按钮按下之后，当帧的 `board_present` 立即返回（不再等掉一场），主循环在一帧内退出，退出码 0；同一帧 `board_keys` 的返回值没有任何一位因为关窗而变化 |

执行方式：第 3、4、5、6 条由测试应用自己判并打印 `fail N`；
第 1 条的对 `golden.txt`、第 8 条的有窗/无窗互比，由 `check_sw4_pc.sh` 跑两次再比；
第 2 条分两半，最近邻由主机侧探针判，窗口切换与按键泄漏要人按；
第 7 条在 `make -f mk/host.mk test` 里；第 9 条要人点窗口的关闭按钮，
应用把每帧的场号与位图打出来，看关窗那一帧之后还有没有多等一场。

**PC 上不判的两条**（D10）：前缓冲在一场内没有被改写、切换不撕裂。
两者都是扫描输出与引擎的性质，PC 侧帧缓冲只有应用一个写者，自证无意义；
它们留在 SW9 与引擎的判据里。

## 4 开发日志

### 2026-09-21 按 2.8 的九步落地，PC 侧判据全部通过

九个步骤全部做完，代码与判据一起入库。下面按「做了什么、红过什么、结果是多少」记。

**1、2 步：录像格式与转换脚本。** `sw/lib/core/replay.{h,c}` 是纯算法（格式、打包解包、
流式录制、逐帧取键、录制文本），两个平台编同一份；`Main/tools/check/replay2c.py`
把 `.g2dr` 与 C 数组、录制文本之间互转，只用标准库。固件
`lib/test/replay_fixture.g2dr`（256 帧、种子 7、1040 字节）入库，
主机测试 `lib/test/test_replay.c` 五组 60 条判据，其中 R5 把三条路对上：
入库的 `.g2dr`、脚本生成的 C 数组、以及按头部种子用 `lib/core` 的 xorshift32 重算再打包的字节，
三者逐字节相同。**这一条同时把脚本里的 xorshift32 与 C 那份对上了**，两边不一致时它会红。

**3~6 步：板级接口与 PC 实现。** `board.h` 补 `board_frame_count`、`board_swap_count`、
`board_should_quit` 三个服务与它们的契约（含「减法要先用 uint32_t 减完」那一句）；
`bsp/ti60` 读 `FRAME_COUNT`/`SWAP_COUNT`、`board_should_quit` 恒 0；
`bsp/host` 按 D1 重写场节拍（自由运行场计数 + 等下一个场边界）、两个计数、关窗退出、
F1~F3 缩放（最近邻 hint 显式设 "0"，缩放键不进位图）、等待用「睡整毫秒减一加自旋」。
headless 一行代码都没加：`SDL_VIDEODRIVER=dummy` 换驱动，跑同一个二进制。
另加主机侧探针 `Main/tools/check/check_sw4_scale.c`。

**7、8 步：判据程序与检查脚本。** `sw/apps/test/platformTest` 一次跑完六个场景；
`Main/tools/check/check_sw4_pc.sh` 把要跑多次再比的几条串起来（有窗、无窗、注错构建各跑一遍）。
`lib/gfx/gfx.h` 里补了 `GFX_CASE_FB_BASE`/`GFX_CASE_TEX_BASE`/`GFX_CASE_DEV_BYTES`
三个常量，主机测试与判据程序共用一份，免得两处各写一遍再漂移。

**9 步：受影响的既有代码。** `gfx2dGame` 删掉多出来的 `board_wait_frame()`；
`sw/README.md`、`doc/开发环境.md`、`tools/check/README.md` 三处登记跟上。

#### 结果

全量跑一次 `check_sw4_pc.sh`（600 帧节拍、1800 帧回放两遍、三次运行）：

| 项 | 结果 |
| --- | --- |
| S1 十一个用例 CRC 对 `golden.txt` | 逐行相同（11/11） |
| S2 有窗与无窗 | 5412 行序列逐行相同；两次跑的是同一个二进制（`sha256:e19e1a473812d0b7`） |
| S3/S4 两次回放与 `fail =0` | `replay diff=0`、`played=1800`，`fail =0`、`done` |
| S5/S6 录制文本与脚本自检 | 1800 帧文本收成 `.g2dr` 通过结构校验；`--selftest` 全过 |
| S7 缩放探针 | 最近邻硬边、两个 hint 结果不同、三种窗口尺寸都成立 |
| S8 注错构建 | `INJECT=REPLAY_CLOCK` 之后两次回放出现不同，`fail =0` |

判据程序自己那几段（PC、x11，空载）：

```text
case 0..10 crc ...            11 行与 golden.txt 逐行相同
pace frames=600 dfield=600 dswap=600 min_us=16516 max_us=16816 missed=0
drop cycles=2000000 dfield=2 dswap=1
replay frames=1800 played=1800 diff=0
fail =0
done
```

`min_us`/`max_us` 空载落在 16.5~16.8 毫秒（一场 16.667 毫秒），与 4.2 节探针的
16.20~17.13 是同一量级；差的不多是探针那一版只在纯计数器循环里量，这一版每帧多了一次
`SDL_UpdateTexture` 与 `SDL_RenderPresent`。**有编译负载时尾巴会到 18 毫秒**，
所以第 3 节那两条已改成宽上下界，见 4.5。

顺带核对既有产物：`gfx2dGame` 的 PC 日志与入库的 `logs/pc_normal.log` 60 个 CRC 逐帧相同
（删掉那次多余的 `board_wait_frame` 不影响 CRC 与行数）；板上产物变成 4210 字节
（`sha256:a8df1ef4b1d5d618`，其中 80 字节是本次三个查询，其余是扫描输出的寄存器通路）。
`make -f mk/host.mk test` 与 `check_sw_layers.sh` 各跑一遍，都是 `fail 0`。

#### 遇到的问题

六处：前三处与第四处是判据自己抓出来的，第五处是判据自己红了之后改判据，第六处是回读代码时发现的假绿。

**4.1 `lib/test/` 必须能在只有 `-Ilib` 的条件下编过，而录像判据要一个生成的头文件。**

第一现场（`check_sw_layers.sh` 第 1 项）：

```text
    编不过：lib/test/test_replay.c
    lib/test/test_replay.c:24:10: fatal error: replay_fixture.h: No such file or directory
1 lib/ 只有 -Ilib 编过 FAIL
```

解读：第 7 条判据要求「录像经脚本变成 C 数组之后」两份位宽解析一致，所以测试要拿到那个数组；
数组是生成物、落在 `$(OBJDIR)/test/` 里、不入库，而结构判据要求 `lib/` 每个文件在只有 `-Ilib`
时也编得过。两个要求本身不冲突，冲突的是「无条件 include 一个生成的头文件」。

定位：先想在 `lib/test/` 里提交这个头（违反「生成物不入库」），再想把第 1 项排除 `lib/test/`
（那是把判据改松，正是不能做的）。最后用了 `__has_include`。

根因与修法：缺头文件时仍然编得过，**缺了就在运行里报 FAIL**，不静默跳过：

```c
#if defined(__has_include)
#  if __has_include("replay_fixture.h")
#    include "replay_fixture.h"
#    define HAVE_REPLAY_FIXTURE 1
#  endif
#endif
```

验证：手工按「只有 `-Ilib`」编一份不带生成头的 libTest 跑，输出是
`R5 没有 replay_fixture.h（先跑 make -f mk/host.mk test 生成它） FAIL` 与 `fail 1`；
`make -f mk/host.mk test` 生成了头，R5 全部 `ok`。**原判据为什么没有覆盖到它：**
第 1 项只编 `lib/` 自己的文件，而录像那一组是新加的，加进来之前没有文件依赖生成物。

**4.2 缩放探针的判据报红，打印出来的像素却是对的。**

第一现场（第一版探针）：

```text
hint 0 (最近邻): x=79:0x000000 x=80:0x000000 x=81:0x000000 x=159:0x000000 x=160:0x000000
S2 hint 0 边界是硬边 FAIL
```

解读：`x=80` 应当是白的，采样全是 0，看着像「最近邻没生效」；
但同时 `hint 1` 那一行给的是 0x2b2b2b，与 4.3 节探针的 0x7f 也对不上。

定位：加一段 `SW4_SCALE_DEBUG` 打印 `sample_row` 内部读回的 `dst`，发现内部是对的
（`row40 x79=0x000000 x80=0xffffff`），错在往 `out[]` 里搬的时候用了
`dst[row * 640 + x]`——`x` 是 0~4 的循环下标，读成了第 0~4 列，而探针要的是
`xs[]` 里的 79/80/81/159/160。**读错的是「搬出来的那一步」，不是渲染。**

根因与修法：`sample_row` 改成收一个列号数组，`out[x] = dst[row * 640 + xs[x]]`。
改完 x11 与 dummy 两支驱动下都是 `fail 0`，值与 4.3 节一致。
**原判据为什么没有覆盖到它：** 这是判据程序自己的缺陷，它报的 FAIL 指向的却是被测量的对象；
只有把「内部读到的值」打出来才能把这两件事分开。

**4.3 日志行尾是 `\r\n`，`diff` 把 11 行全报成不同。**

第一现场：

```text
1,11c1,11
< fill-screen 9cd34cb7
---
> fill-screen 9cd34cb7
```

解读：两边看着一模一样，`diff` 却说全不同。原因是应用走 `board_puts`，
行尾是 `\r\n`（与板上 UART 逐字节相同的约定），`golden.txt` 是 `\n`。

根因与修法：`check_sw4_pc.sh` 里比对前一律先 `tr -d '\r'` 归一。
**原判据为什么没有覆盖到它：** 这条对账是新加的，之前没有哪一步把两边的行尾放在一起比过。

**4.4 `lib/core/replay.c` 用了 `NULL` 却没包含 `<stddef.h>`。** 主机侧一编就报
`'NULL' undeclared`。`lib/` 走的是 freestanding 那套，头文件包含要自己写全，不靠别的头捎带。

**4.5 帧节拍的挂钟上下界在编译同时跑时会红，而场数增量一次都没错过。**

第一现场（全量跑 `check_sw4_pc.sh` 的 S4）：

```text
S4 有窗运行的 fail 不为 0 FAIL
    28:chk P6 最长间隔不超过 18 毫秒 FAIL
```

解读：同时在跑板上构建（gcc 在同一个 WSL 里编），单独看这一帧的间隔超过了 18 毫秒。
同一份输出里 `missed=0`、`dfield=600`、`dswap=600` —— **模型一条都没错**。

定位：重复跑六轮、每轮同时在后台编一次 `gfx2dGame`，量到的上下界是
`16.5~16.8`（空载）、`15.3~18.0`、`15.7~17.6` 毫秒（有编译负载）。尾巴是宿主调度给的。

根因与修法：把挂钟那两条从「结构性判据」降成「数量级检查」（下界 10 毫秒、上界 25 毫秒），
结构判据归 P1「每一帧的场数增量都是 1」：**「立即返回」会让某帧的增量变成 0，
「等两场」会变成 2，两种坏法都被 P1 挡住**，而它只看计数器、与调度无关。
第 3 节那一条已按此改写。**原判据为什么没有覆盖到它：**
第 3 节的上下界是照 4.2 节探针的空载数字（16.20~17.13）写死的，而探针跑的时候机器是空的。

**4.6 中途关窗会让两串没跑完的回放互比，得出「相同」这个假绿。**

解读（回读代码时发现的）：`live` 段中途关窗之后 `s_quit` 一直是 1，
接着的回放段每一帧都在第一次检查处直接退出，两串 CRC 数组都停在同一个前缀上，
后面的元素还都是上一次的零，于是 `diff=0`、`chk R3 两次回放逐帧相同 ok`。
**一个真实的提前退出会伪装成判据通过。**

根因与修法：`play_pass` 返回真正跑完的帧数，两遍都跑满（1800 帧）才比对，否则报
`chk R3 两次回放都跑满 FAIL`；`main` 在每一段之后看一次 `board_should_quit()`，
关过窗就把剩下的场景明说没跑（`quit：关窗之后剩下的场景没有跑`），不留半截结果当结论。

#### 还欠什么

- **板上那三个查询没有上板跑过。** `bsp/ti60` 的 `board_frame_count`/`board_swap_count`/
  `board_should_quit` 只验到「真实板上构建编得过、产物 4210 字节」，
  板上判据没重跑。归 [SW9](SW9-板上移植.md)：它本来就要在板上量帧节拍，
  顺手把两个计数与掉帧减法在板上成不成立一起验。
- **第 9 条（关窗退出）要人动手。** 自动化的三次运行里没人点关闭按钮，
  只跑到「没有关窗」那个分支并打 `live 没有关窗`；`chk L1 关窗当帧没有等场` 与
  `live quit_df=0` 要人点一次才会出现。关过窗之后 `main` 会把剩下的场景明说没跑
  （4.6 节）。同样要人动手的还有 F1~F3 切窗口（`keys` 那一列必须一位都不变）。
- `gfx2dGame` 的板上日志没有重采。它采于扫描输出之前，`board_present` 当时是空操作，
  60 个 CRC 仍然有效；真要重采时按 `displayTest` 那份比特流走（`sw/README.md` 已写明）。

### 2026-09-21 前期研究复核：四处补漏

对着前期研究的结论逐条回读代码，D1~D10 的定案与 4.1~4.5 的数据都复核无误，
补了四处研究没有覆盖、而开工路上会挡路的事：

1. **关窗口退不出去**（2.2 节末、2.7 表、第 9 条判据）。`s_quit` 现在只让
   `board_wait_frame` 提前返回，应用拿不到。present 按 D1 改成阻塞等场边界之后这件事更重：
   关了窗口每帧照样各等掉一场。补 `board_should_quit()`，并规定置位后两个等待都立即返回、
   退出不进按键位图。十项取舍里没有这一项，`board.h` 的服务清单也漏了它。
2. **headless 不需要编译宏**（D5 补充段、2.5 节、第 8 条判据）。D5 论证的是「**应用自己的**
   开关不能靠 argv/env，因为板上没有来源」，这对；但 `SDL_VIDEODRIVER` 是 SDL 库读的宿主配置，
   `bsp/ti60` 根本不链接 SDL，不在这条的适用范围内——4.4 节那三组数据本来就是在 shell 里
   给环境变量跑出来的。改用环境变量还修好了一处自相矛盾：编译宏会让有窗与无窗成为两个
   不同的二进制，而 D3 选 dummy 的全部理由就是「同一条执行路径」。
3. **两个增量之差要在 32 位上算完**（2.2 节）。4.2 节旧算法那行的 `drop=2^64-1`
   是探针在 64 位上做减法的产物；接口契约是 32 位回绕，按契约应是 2^32−1。
   数字本身不影响 D1 的结论，但 SW8 要按秒累加掉帧数，先提升再减会把一次回绕放大成
   天文数字，整列作废，所以把减法的写法写进契约。
4. **十一用例的落点定死**（2.8 第 7 步）。`gfx_case_build` 的 `fb`/`tex` 是参数、
   `gfx_mem` 是 `{dev_base, host, bytes}` 窗口，所以 `lib/gfx` 一行都不用改；
   要定的是窗口取哪一段。定成「fb 与 tex 都放帧缓冲 0 分区」：分区 4MB，画面占前 1.875MB，
   纹理 16KB 放偏移 `0x20_0000`，两边都不必把整块 DDR 的基址暴露给 `lib`。

### 2026-09-21 前期研究：环境取样、场节拍模型与十项取舍

本轮不动产品代码，做四件事：读既有代码与扫描输出的 RTL、把本机 SDL2 的环境事实取下来、
用探针对比两种场节拍模型、把结论落成 1.1 节的 D1~D10 与第 3 节的量化判据。
探针是一次性的、没有入库；下文的数据是它的原始输出，做法在 4.3、4.4 两节写全，
照做法可以重跑（4.3 的最近邻检查在 2.8 节第 5 步会沉淀成主机侧探针）。

#### 4.1 环境事实（本机实测）

```text
WSL Ubuntu 26.04  gcc 15.2.0  sdl2-config 2.32.10  python3 3.14.4
SDL runtime 2.32.10 / compiled 2.32.10    （运行时与编译期同版本）
SDL_GetPerformanceFrequency = 1000000000
DISPLAY=:0   WAYLAND_DISPLAY=wayland-0     驱动 in use = x11
display 0 = 2560x1600 @ 60 Hz fmt=SDL_PIXELFORMAT_RGB888
video drivers: x11 / wayland / KMSDRM / offscreen / dummy / evdev
hint SDL_HINT_RENDER_SCALE_QUALITY default = (null)     即未设
gcc -m32 编得过（gcc-multilib 在，第 7 条判据的前置成立）
SDL_Delay(1) 200 次：min=1058 us avg=1092 us max=1149 us
```

两件事直接决定写法：**性能计数器的频率是 1e9**，`step = freq / 60 = 16666666`，
折算出的场频是 60.0000024 Hz（每天差 0.014 秒，可以忽略）；
**`SDL_Delay` 的最小粒度约 1 毫秒**且稳定超出 60~150 微秒，所以等场不能用纯睡眠。

#### 4.2 场节拍：两种模型的实测对比

探针把 600 帧空渲染跑一遍，第 100 帧人为 `SDL_Delay(20)` 造一次掉帧，
三种后端各跑一次，读数是 `board_present` 返回时刻的间隔与两个计数器的增量。

旧算法（SW1 `board_wait_frame` 的 `s_next_frame += step` 加落后对齐）：

```text
old_grid(SW1) x11     n=600 min=1112 us avg=16642 us max=21144 us
                      frames=600 swaps=600 delta_frame=599 delta_swap=600 drop=2^64-1
old_grid(SW1) dummy   n=600 min= 204 us avg=16640 us max=20383 us
old_grid(SW1) novideo n=600 min=   4 us avg=16639 us max=20124 us
```

新模型（D1 的自由运行场计数 + 等下一个场边界）：

```text
hybrid(delay+spin) x11     n=600 min=16201 us avg=16694 us max_ok=17131 us
                           注错帧 33333 us (2.000 场)
                           delta_frame=601 delta_swap=600 drop=1 extra_missed=0
hybrid(delay+spin) dummy   min=16495 avg=16694 max_ok=16903   注错帧 33333 us  drop=1
hybrid(delay+spin) novideo min=16589 avg=16694 max_ok=16727   注错帧 33410 us  drop=1
spin_only        x11       min=15148 avg=16694 max_ok=18185   注错帧 33333 us  drop=1
```

读数说明：

- 旧算法的最小间隔 1112 微秒（dummy 与 novideo 下只有 204 与 4 微秒）说明它**落后之后会
  连续几帧立即返回**；`delta_frame=599` 小于 `delta_swap=600` 说明场号已经不再与场节拍同源，
  掉帧减法回绕。它只能保证平均节拍，不能拿来算掉帧。
- 新模型在三种后端下的注错帧都正好是 **2.000 场**，掉帧减法得 1，正常帧的间隔在
  16.20~17.13 毫秒（0.97~1.03 场），`extra_missed=0` 表示没有一帧多掉一场。
- 纯自旋与混合等待的差别只在 15.15~18.19 与 16.20~17.13 毫秒之间，混合写法还省 CPU，
  所以取混合（D3 之外的另一处小定案，不单列取舍项）。

#### 4.3 缩放、窗口尺寸与最近邻

```text
CreateRenderer(ACCELERATED) = ok        驱动 x11
window 640x480 ok / 1280x960 ok / 1920x1440 ok      display 0 = 2560x1600，3x 放得下
scale_quality='0' -> x=79:0x000000  x=80:0xffffff  x=81:0xffffff  x=159:0xffffff  x=160:0x000000
scale_quality='1' -> x=79:0x7f7f7f  x=80:0x808080  x=81:0x828282  x=159:0x808080  x=160:0x7f7f7f
```

做法是把 8×8 的黑白棋盘放大 80 倍铺满 640×480，再用 `SDL_RenderReadPixels` 读回第 40 行。
最近邻下 79/80 两个相邻像素是黑白硬边（一个源像素正好铺满 80 个屏幕像素），
线性下边界两侧都是插值出来的 0x7f/0x80。`SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY)`
返回 NULL，所以必须显式设。

#### 4.4 无窗口的三种做法

```text
SDL_VIDEODRIVER=dummy   驱动 in use = dummy
                        CreateRenderer(ACCELERATED) = Couldn't find matching render driver
                        CreateRenderer(SOFTWARE)    = ok
                        CreateTexture(XRGB8888,STREAMING,640x480) = ok
                        节拍与窗口组逐条一致，注错帧仍 33333 us
SDL_Init(0)             驱动 = (null)，视频子系统没起
                        节拍一致（min=16589 max_ok=16727），但窗口、渲染器、纹理三处都要判空
去掉 DISPLAY / WAYLAND_DISPLAY / XDG_RUNTIME_DIR 后按原样跑
                        驱动 in use = offscreen（SDL 自己挑的）
                        SDL_Init(VIDEO) 与 CreateRenderer(ACCELERATED) 都成功
```

最后一条推翻了原文 2.5 节的动机：无显示环境下 `board_init` 不会退出。
真正的问题是不指定后端时落到哪个驱动由环境决定，三个后端的渲染器与阻塞行为不同，
headless 与有窗因此不是同一个执行路径。已按 D3 改成显式 dummy。

#### 4.5 板上语义的原文核对

- `rtl/display/display_fetch.v` 第 344~357 行：`swap_req_sh` 是粘滞请求位，
  软件写 1 后由场起始锁存并清零，所以请求不会漏、不会攒账。D1 的「等下一个场边界」成立。
- 同文件第 411 行：`frame_count` 在 `fs_event` 上无条件加一，与 `state`、`EN` 都无关，
  所以它是自由运行的场计数，PC 侧照此建模。
- 同文件第 389~393 行：`swap_count` 在场起始之后的第六步（`S_WAIT`）里加一，
  所以板上 present 的返回时刻比场起始晚几十个 `core_clk`。作为已知建模误差记在 2.2 节。
- `sdk/software/standalone/common/start.S:81` 是 `call main`，没有 argc/argv。D5 的依据。
- 十个应用的入口全是 `int main(void)` 或 `void main(void)`；`lib/test/test_main.c` 用了 argv，
  但 `lib/test/` 不进应用的库列表。D5 的依据。

#### 4.6 本轮改了什么

1. **2.2 节的等待模型换成 D1 的自由运行场计数 + 等下一个场边界**，
   并给出取两次读数相减之外的精确措辞：`board_frame_count` 任何时刻可读，
   `board_swap_count` 只在 present 返回后增加。
2. **2.2 节补上 PC 侧两个查询的来源与板上建模误差**（六步清空带来的两微秒）。
3. **2.4 节撤掉「文件读写归 `bsp/host`」**，改成回放一律读编译进程序的 C 数组、
   录制结果就是日志、由 `replay2c.py` 转换（D7）。原文的分配会让两边共编的应用
   依赖只有一边有的服务。
4. **2.4 节补上录像格式的定稿**（D6）与注错判据的具体做法。
5. **2.5 节订正动机**：无显示环境下 SDL 不会失败，问题是后端不受控（4.4 节）。
6. **新增 2.7 节**：受影响的既有代码逐条列出，其中 `gfx2dGame` 的一次多余
   `board_wait_frame()` 必须删掉。
7. **新增 2.8 节**：开工顺序，1、2 步与 3~6 步可并行。
8. **第 3 节的判据全部量化**并写明适用范围（D10），删掉原文「掉帧后前缓冲没有被改写」
   这一句在 PC 上的落点。
9. **新增 1.1 节的十项取舍**（D1~D10），每项给结论与代价。

本条**不欠**新的 Todo 条目：前期研究里没有发现需要独立跟踪的缺陷，
唯一一处跨条目的影响（`gfx2dGame` 在板上的帧率与那份日志的比特流）已写进 2.7 节，
随本条一起改。
