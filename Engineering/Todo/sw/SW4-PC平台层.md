# SW4 PC 平台层

状态：**进行中，前期研究完成，可开工**（2026-09-21）。前置 SW1 已于 2026-09-19 验收：
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

**结论：headless = 在 `SDL_Init` 之前把 `SDL_VIDEODRIVER` 置成 `dummy`。**
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
headless 用编译宏 `HEADLESS=1`（两套构建都认，PC 上用它跑第 8 条判据）。
互动开发（SW6、SW7）本来就不需要开关：游戏应用开窗直接跑。

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

`--headless`（实现上是编译宏 `HEADLESS=1`，见 D5）要求在**没有显示的环境**里跑完，
做法见 D3：`SDL_Init` 之前把 `SDL_VIDEODRIVER` 置成 `dummy`，之后一行分支都不加。

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
| `sw/bsp/include/board.h` | 补 `board_frame_count` / `board_swap_count` 的声明与契约 | 2.2 节，两个平台都要实现 |
| `sw/bsp/host/board.c` | `board_present` 改阻塞、`board_wait_frame` 换成本条的原语、加两个查询、缩放键与窗口尺寸、headless 驱动 | 本条主体 |
| `sw/bsp/ti60/board.c` | 加两个查询的实现（读 `DISP_FRAME_COUNT` / `DISP_SWAP_COUNT`） | 同一个接口的板上实现 |
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
6. headless：第 8 条判据。
7. 测试应用 `sw/apps/test/platformTest`：整帧十一用例、600 帧节拍、注错掉帧、录制与两次回放。
8. `check_sw4_pc.sh` 把第 1、8 条判据串起来。
9. `gfx2dGame` 的调用序列与 `sw/README.md`、`doc/开发环境.md` 的登记。

第 1、2 步与第 3~6 步互不依赖，可以并行。

## 3 判据

八条判据都在 PC 一侧：第 7 条在主机侧单元测试里（不经 SDL），第 2 条的最近邻一半在主机侧探针里，
其余跑真实应用。**原文「掉帧」里「前缓冲在这一场内没有被改写」半句已删**，理由见 D10。

| 项 | 判据 |
| --- | --- |
| 显示 | 跑 `sw/lib/gfx` 的十一个整帧用例（SW3 已入库），每帧 CRC 命中 `sw/lib/test/golden.txt`；同时目测窗口里没错位，并对首尾两行逐像素比对，判行跨 4096 有没有接错 |
| 缩放 | 1×、2×、3× 三种窗口下同一帧的 CRC 逐位相同；最近邻由主机侧探针读回边界像素判（做法见 4.3 节，沉淀成 `tools/check/` 下的探针）；**缩放键不进位图**照 `keyTest` 的做法要人按：应用把每帧位图打出来，按 F1~F3 的窗口期内这一列必须一位都不变 |
| 帧节拍 | 连续 600 帧空渲染，**每一帧的场数增量都是 1**，`board_frame_count` 与 `board_swap_count` 的增量都是 600，相邻两次 `board_present` 返回的间隔落在 15.0~18.0 毫秒（实测 16.20~17.13） |
| 掉帧 | 人为让某一帧渲染耗时 20 ms，该帧场数增量为 2 而切换数增量为 1，应用算出的掉帧数加一 |
| 输入 | 所有键位在日志里打印位图，逐键核对；回放段不读键盘，所以回放那次位图列必须与录像里的按键序列逐帧相同 |
| 确定性 | 一段 30 秒的录像（1800 帧）回放两次，两次的 1800 行 CRC 逐行相同；注错：让应用读一次 `board_cycles()` 并参与画面计算，两次的序列必须出现不同 |
| 录像格式 | 同一份录像经 `replay2c.py` 变成 C 数组后，`-m32` 与 `-m64` 两份主机测试解析出的按键序列逐帧相同，且与直接解析 `.g2dr` 的结果相同 |
| 无窗口回放 | `HEADLESS=1` 且去掉显示相关环境变量时跑完，退出码 0，每一行 CRC 与有窗模式逐帧相同 |

执行方式：第 3、4、5、6 条由测试应用自己判并打印 `fail N`；
第 1 条的对 `golden.txt`、第 8 条的有窗/无窗互比，由 `check_sw4_pc.sh` 跑两次再比；
第 2 条分两半，最近邻由主机侧探针判，窗口切换与按键泄漏要人按；
第 7 条在 `make -f mk/host.mk test` 里。

**PC 上不判的两条**（D10）：前缓冲在一场内没有被改写、切换不撕裂。
两者都是扫描输出与引擎的性质，PC 侧帧缓冲只有应用一个写者，自证无意义；
它们留在 SW9 与引擎的判据里。

## 4 开发日志

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
