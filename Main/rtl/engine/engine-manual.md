# engine 设计文档

引擎 RTL（`rtl/engine/`）的随码文档：模块清单、编码表、注错开关与关键协议。
寄存器偏移与位号的单一真源是 `engine_regs_offsets.vh`（`tools/check/regs_gen.py` 生成），
命令语义的权威是 `doc/赛题二总体设计.md` 3.4 节，层间协议见本文第 4 节。

## 1 模块清单

| 模块 | 职责 | 备注 |
| --- | --- | --- |
| `engine_top` | 对外唯一入口：APB 分选（第 0 页寄存器、第 4~7 页 TEX）、旧命令层与 PB 前端二选一 | 接 Axi_Mux S1 |
| `engine_regs` | APB 第 0 页寄存器、PERF 计数、软复位握手 | 生成偏移，不手写数字 |
| `engine_cmd` | 命令表取数、十步检查、串行下发；含 `ec_struct_ok`/`ec_fields` | 检查逻辑权威 `sw/lib/gfx/check.c` |
| `engine_geom` | 五者求交、逐行描述符与边带 | 只碰坐标地址 |
| `engine_pix` | 行缓冲、两拍窗口对齐、12 个混合核 | 只碰像素 |
| `engine_blend` | 单通道混合核（6 方程，3 级流水） | 例化 12 份 |
| `engine_mover` | 唯一 AXI 主机：仲裁/门控/排空/超时/带内掩码 | 挂 `~rst_ddr_n` |
| `engine_tex` | 16 KiB 片上纹理，四 bank | APB 第 4~7 页 |
| `engine_band` | 条带数据面 640×STRIP_H×32 | 单缓冲，三相循环 |
| `engine_inst` | v1-I 实例坐标取数（256×32 缓存） | FD 流与命令取数共用 |
| `engine_pb` | PB 帧控制与引用执行（31 状态） | bin_mode=1 时接管 |
| `engine_bin` | PB 两遍构建：COUNT/PREFIX/SCATTER/目录 | 32×96 桶 RAM |
| `engine_packet_check` | PB 构建期单包预检 | 复用 ec_* 谓词 |
| `engine_bounds` | 18 位有符号五者求交 → 条带区间 | 组合 |
| `engine_pb_dma` | 16/32/64 B 小事务桥 | PB 写独立 owner |

## 2 六方程编码（engine_blend）

恒等式：`x = base×255 + m1×m2`，`out = min(255, (x + 128 + (x+128 >> 8)) >> 8)`。
代入表与逐位等价由 `tools/check/check_subst.c` 对全定义域穷举（六方程 50462976 组）。

| eq | 名称 | Fs, Fd | base | m1 | m2 |
| -- | --- | --- | --- | --- | --- |
| 0 | COPY | 255, 0 | src | 0 | 0 |
| 1 | ALPHA 预乘 | 255, 255-a | src | dst | 255-a |
| 2 | ADD | 255, 255 | src+dst | 0 | 0 |
| 3 | CONST | c, 255-c | dst | src-dst | c |
| 4 | MUL | dst, 0 | 0 | src | dst |
| 5 | ALPHA 非预乘 | a, 255-a | dst | src-dst | a |

base 9 位无符号（ADD 上限 510）、m1 9 位有符号、m2 8 位无符号零扩展进乘法器。
ALPHA 预乘不限定 src ≤ a：BLIT 的源像素来自表面，没有逐个检查。

## 3 错误与 PERF 编码

错误码（`EC_ERR_*`，落在 STATUS[7:4]）：0 OK、1 BAD_OPCODE、2 BAD_FIELD、3 BAD_EQUATION、
4 BAD_SURFACE、5 BAD_SPACE、6 BAD_FORMAT、7 OVERLAP、8 BAD_PREMULT、9 FETCH、
10 BUS_READ、11 BUS_WRITE、12 TIMEOUT。

ERROR_STAGE：0 旧路径/无错、1 包级、2 实例、3 分桶计数/容量、4 PB 写、5 引用执行、6 BAND。

PERF 下标 0~8 为原九项（CMD/PIXEL/KEY_SKIP/RD_BURST/WR_BURST/RD_BEAT/WR_BEAT/
WAIT_DDR/BUSY），9~13 为实例命令与硬件分桶前端追加（INSTANCE/REF_COUNT/BIN_CYCLES/PB_RD_BEAT/PB_WR_BEAT）。

## 4 关键协议

### 4.1 命令层取数契约（engine_cmd）

- 非空命令表要求 LIST_BASE 按 32 字节对齐，违例在取第 0 条前报 FETCH；
  搬运层读核 ENABLE_UNALIGNED=0 会把未对齐地址向下取整，不能静默读错。
- 一批最多 16 条、512 字节；跨 4KB 在记录边界拆两笔描述符，任一笔失败整批丢弃、
  从批首逐条回退（回退保持顺序执行的语义：出错点之前的记录照常执行）。
- 记录地址 base+32×index 按 38 位求值；越过地址空间的记录不取不回卷，执行需要时报 FETCH。
- `fd_cidx`/`fs_cidx` 只有 4 位且是描述符上下文（批首），完整 ERROR_INDEX 由本层重建；
  `fe_cidx_err` 只作一致性监视。
- 预取只缓存：预取批数据与状态收齐才标记 OK/ERR；当前批先遇 END 或错误时丢弃预取结果，
  预取的 SLVERR 不上报。物理预取可能读到 END 之后（命令表在 DDR 上无读副作用），不查不报。
- 提交纪律：一次只给几何层一条 FILL/BLIT；非空命令等带 wr_last_row 的写状态、
  空命令等 cmd_done、NOP/SET_CLIP 当拍完成。活动命令任何一笔读写状态报错或超时立即上报
  并停止推进，等在途命令收尾、预取流收完才回空闲。
- 读状态按搬运层显式来源位 `rd_stat_fetch` 分发：取数来源归取数引擎，
  像素/TEX 读恒归活动命令；写状态只来自像素通路，恒归活动命令。
- v1-TEX：BLIT 源允许 space=1（TEX），目的必须 DDR；TEX 源结构多查 base 16 字节对齐与
  `base + height×2^stride_log2 ≤ 0x4000`（18 位端点，完整行跨含 padding）。

### 4.2 搬运层（engine_mover）

- 每方向最多一笔在途。读方向「完成」= 状态脉冲已捕获**且**读流 tlast 已握手：
  读核状态脉冲可早于末拍握手 1~55 拍，只看状态放行会把上一个描述符的尾拍算进下一个。
- 写流 tlast 由本层打：写核在拍数用完而流上没见到 tlast 时进丢数据态，下一行写流会被丢。
  tlast = 「剩下的最后一拍」，正常写流与排空哑拍同一个式子；一行跨 4KB 时段界只在
  写描述符上，计算阵列算不出来。
- 排空：drain_req 电平请求。读方向把 tready 恒 1 吃掉在途数据；写方向按
  「描述符拍数 − 已送拍数」灌哑拍（像素标记位全打不写 → wstrb=0）。两个核加 TEX/BAND
  都空闲后发一拍 drain_done。一次请求只完成一次：drain_req 举着的每一拍都不收新提交。
- 超时只报告不放弃：在途且连续 T 拍无任何握手打一拍 tmo_evt；「握手」两侧都算
  （本层描述符/流/状态 + AXI 一侧），否则空档会跨过核内 32 深输出 FIFO。T 复位值 4096。
- 带内掩码：像素 X 字节最高位 = 不写标记，本层按位清 wstrb 并把 X 字节清零上总线。
  写核中间拍 wstrb 恒全 1、不看 tkeep，掩码只能在核外按位清。BAND/原始写取消时按
  已入写核 FIFO 的合法前缀在 AXI 出口施杀（写核整拍不保留输入 keep）。
- 上下文寄存器：目标缓冲号、命令索引、来源（命令/实例/背景）每方向一组；不用核的 4 位 tag。
- TEX/BAND 接入：描述符入口按 rd_tex/rd_band 选路；TEX/BAND 只在上一笔 DDR 读完整收尾后
  受理，返回流保持单所有者（共享行写指针不能在源/目的间交错）。状态合并带显式来源位，
  本地状态遇同拍 DDR 状态时保留到下一拍。

### 4.3 计算阵列（engine_pix）

- 流水 7 拍：0 地址 → 1 行缓冲读出 → 2 数据级 → 3 旋转/色键/a/掩码 → 4~6 混合核 → 7 合并不写标记。
- 旋转：`off = (ls >= ld)`，`st = (ls - ld) mod 4`；off=1 的行首先走一拍预热（地址 0 压进读口）。
- 首末拍掩码：目的拍 k 槽 t 的像素号 j = 4k + t − ld，属于本行 ⟺ 0 ≤ j < npx；
  一拍的行把行尾算进首拍掩码。色键比较源原始 32 位存储字，只对 BLIT；
  行外或色键命中的槽在 X 字节最高位打 1。
- a 的来源：BLIT 源格式 XRGB 取 255，其余取源字高字节；FILL 一律取 `w4[31:24]`
  （FILL 的 a 就是常量字高字节），不看边带 sfmt。
- 时钟使能 `en = ws_tready || !ws_tvalid` 管住每一级（含行缓冲读口与 12 个核）；
  行内下游 ready 不出气泡，一行发完等 8 拍排空再开下一行。
- 行队列按边带握手先后记下半号；一半在写流末拍被写核收走前不再收边带（row_free 反向握手）。

### 4.4 几何层（engine_geom）

- 五者求交：目的矩形、屏幕 [0,639]×[0,479]、clip、目的表面、源表面（FILL 无源）。
  源表面项 = 「源越界只跳过该像素」的矩形级等价，交掉后像素通路不再逐像素判。
- 描述符按 16 字节对齐覆盖区间发，可能带出界外像素（带内标记不写）；跨 4KB 与
  MAX_BEATS=192 拍拆段，一行最多两段。
- 发出顺序每行 = 边带 → 读源 → 读目的 → 写目的；rd_buf = {源/目的, 行号最低位}。
- row_free 电平握手，每行发边带前查（含每条命令第 0 行）；计算阵列在该半边带握手后拉低、
  写流末拍被写核收走后拉高。

### 4.5 片上纹理（engine_tex）

- APB 只收 4 字节对齐 32 位访问；读 SETUP 相发宽读、ACCESS 相给数（pready 恒 1），
  写在 ACCESS 完成沿提交；未对齐回 PSLVERR 不改写 RAM。
- 宽读口读流优先，同拍竞争时 APB 读拿不到口、读回值不保证——运行期 CPU 不得读写 TEX
  是驱动纪律，硬件不加互锁。
- 消费者恒 ready（无 ready 口、无 skid buffer）；描述符恰好一笔的纪律由几何层保证，
  本模块把地址截到窗口内使用，越界是上游检查的责任。

## 5 注错开关（变异测试）

正常编译不定义任何 `INJ_*`（预处理删除，不依赖综合器优化）；一次只开一个，
用例与各 `sim/engine/*/run.sh` 一一对应，变异必须在指明的消费者处见红。

| 开关 | 注入缺陷 |
| --- | --- |
| `INJ_WRONG_EQ`（blend） | CONST 用 base=src、m1=dst-src 代入，恒等不成立 |
| `INJ_TRUNC16`（blend） | 中间量截到 16 位丢进位 |
| `INJ_UNREG_C`（blend） | 只寄存乘积，级 2 相加两拍操作数 |
| `INJ_COPY_DST`（blend） | COPY 的 base 误取 src+dst（不用的输入会变才看得出） |
| `INJ_X_NO_EN`（blend） | 级 2 不看 en |
| `INJ_VALID_EARLY`（tex） | valid 提前一级与 data 错拍 |
| `INJ_WR_LOOSE`（tex） | 未对齐 APB 写放行 |
| `INJ_STAT_DROP`（tex） | 状态脉冲吞掉 |
| `INJ_SKIP_SURF`（geom） | 漏掉第五项求交（源表面） |
| `INJ_NO_SPLIT`（geom） | 跨 4KB 不拆描述符 |
| `INJ_ALPHA_AS_COPY`（geom） | ALPHA 也走 COPY 的不读目的避让 |
| `INJ_COPY_READ`（geom） | COPY 也发读目的描述符（边带仍说不读） |
| `INJ_ROW_NOFREE`（geom） | 行开始前不看 row_free |
| `INJ_BAND_CLIP`（geom） | 漏掉当前条带求交 |
| `INJ_ROT_OFF`（pix） | 旋转错一个位置（st 多加 1） |
| `INJ_SKIP_KEY`（pix） | 跳过色键，命中像素照写 |
| `INJ_DROP_MASK`（pix） | 丢首拍掩码，行首行外像素写坏 |
| `INJ_EN_NOTREADY`（pix） | en 恒 1，写核停顿丢拍 |
| `INJ_CORE_NOEN`（pix） | 混合核不随流水停 |
| `INJ_NO_GATE_RD`（mover） | 去掉读门控 |
| `INJ_NO_GATE_WR`（mover） | 去掉写门控 |
| `INJ_MASK_BYPASS`（mover） | 带内掩码旁路，wstrb/X 字节直通 |
| `INJ_DRAIN_BYPASS`（mover） | 排空期不帮忙收/补 |
| `INJ_DRAIN_LEVEL`（mover） | 排空按电平重进 |
| `INJ_NO_TLAST`（mover） | 正常写流不打 tlast |
| `INJ_TX_NOWAIT`（mover） | 去掉目的流等待，长行抓流碰撞 |
| `INJ_INST_OWNER`（mover） | 实例分流位恒 0 |
| `INJ_BAND_FLOW_WAIT`（mover） | 去掉 BAND 流等待 |
| `INJ_BAND_RAW_MASK`（mover） | 原始写也施像素掩码 |
| `INJ_BAND_DRAIN_MASK`（mover） | 排空杀前缀失效 |
| `INJ_CHK_SWAP`（cmd） | 对调 space/format 两步的错误码 |
| `INJ_NO_FB`（cmd） | 关掉逐条回退 |
| `INJ_INST_XYUV`（cmd） | 实例展开 xy/uv 对调 |
| `INJ_INST_EARLY_DONE`（cmd） | 实例包提前置 done |
| `INJ_INST_STATE`（cmd） | 活动期间改写实例包状态字 |
| `INJ_STRIP_LOCAL_Y`（cmd） | 条带模式 dst_y 不加 band_y0 |
| `INJ_STRIP_PERF`（top） | PERF 读事件漏掉 BAND 过滤 |
| `INJ_STRIP_HEADER_OFFSET`（cmd） | 条带表基址偏移算错 |
| `INJ_INST_SKIP`（inst） | instance_id 每次加 2 |
| `INJ_INST_STRIDE`（inst） | 游标固定步进 4，不按 stride |
| `INJ_BIN_CLIP`（bin） | clip 快照取场景末次值 |
| `INJ_BIN_ORDER`（bin） | 引用序号偏移 1 |
| `INJ_BIN_SKIP`（bin） | 第二遍跳过实例包 |
| `INJ_BIN_READY`（bin） | READY 状态值错（2 而非 1） |
| `INJ_BIN_HIGH_BYTE`（bin） | 引用高位清零 |
| `INJ_BIN_STALE`（bin） | 双帧复用不重清桶表 |
| `INJ_BAND_EARLY_FLUSH`（band） | flush 不等本地流收尾 |
| `INJ_BAND_FLUSH_ADDR`（band） | flush 地址偏 16 字节 |
| `INJ_BAND_PIXEL_MASK`（band） | bit31 不写标记失效 |
| `INJ_BAND_ADDR_STRIDE`（band） | 局部地址按固定 256 词行距换算 |

注错机制的标准做法见 `Main/doc/开发环境.md` 0.3 节。

## 6 排版约束

综合器（Synplify）对源码文本敏感：token 逐字等价的排版重排会改变映射结果。
`engine_bin.v` 的正文曾因重排使整机映射 +1606 XLR（60064→61670，超过 Ti60F225 的
60800 上限，pnr 拒绝；端口声明与头注释的重排无影响），故该文件正文保持原排版，
注释改造只动头注释与端口块；重排其他模块后必须以整机综合复核资源。

同一现象在**同一源码重复综合**下也有量级：七个代表档跑两轮，XLR 跨轮差 −1059~+1121
（约 2~3%），而 RAM 与 DSP 两轮完全一致。所以绝对 XLR 只在同一轮内可比，
报某档余量必须用同轮读数，或同轮多次综合取区间。

## 7 可选机制开关

### 7.1 宏与依赖

九个自写文件都 `` `include "engine_feature.vh" ``（生成物，见 7.5 节）。
宏是**存在式**的：`` `ifdef GFX_HAS_xxx `` 命中即该机制存在，不定义即整块离开编译输入。

| 位 | 宏 | 依赖 |
| --- | --- | --- |
| 结构 | `GFX_HAS_TEX` | 无 |
| 结构 | `GFX_HAS_BAND_HW` | 无 |
| 结构 | `GFX_HAS_STRIP` | `BAND_HW` |
| 结构 | `GFX_HAS_INST` | 无 |
| 结构 | `GFX_HAS_PB` | `BAND_HW` 与 `INST` |
| 像素 | `GFX_ALPHA` | 无 |
| 像素 | `GFX_EQ_EXT` | 无 |
| 像素 | `GFX_KEY` | 无 |
| 交付 | `GFX_DELIVERY` | 要求 `ALPHA` 与 `KEY` |

有三条依赖边：`PB → BAND_HW`、`PB → INST`、`STRIP → BAND_HW`。
合法配置是向下封闭集，结构位 32 种输入中合法 16、非法 16。
计数与依赖的单一真源是 [`../../tools/check/feature_config.py`](../../tools/check/feature_config.py)，
导入时自检，改动依赖会在那里停住。

### 7.2 关闭态契约

| 项 | 契约 |
| --- | --- |
| 保留寄存器 | 偏移与位号不变，读回 0 |
| 关闭机制对应的寄存器 | 从读/写映射摘掉：读报错、写拒绝 |
| CTRL 不支持的位 | 写入该位的整次访问拒绝，且不改其它字段 |
| 无实例 | `ERROR_INSTANCE` 恒 `0xFFFFFFFF`，`PERF[9]` 恒 0 |
| 无 PB | `PERF[10..13]` 恒 0；INST 仍开时 `PERF[9]` 正常 |
| 无 TEX | BLIT 源空间只允许 DDR(0)，`space=1` 归 `BAD_SPACE` |
| 无色键 | 请求色键的 BLIT/实例包归 `BAD_FIELD`，不能静默执行普通 BLIT |
| 条带共享参数 | `LSTRIPS/STRIP_H/BAND_SURF` 在 STRIP 或 PB 存在时映射；两者都无时读写报错 |
| 无 STRIP | 独立 `CTRL.BAND` 写拒绝；PB 存在时允许同次写 `BAND|BIN`，由 PB 控制条带 |
| 无条带模式 | `CAPS` 不变（无专属位），只能靠「无条带表取数」与「CTRL.BAND 被拒」判 |

### 7.3 能力位与默认后端

`CAPS` 是能力掩码：它说明这块硬件**具备**什么，不说明默认走哪条后端。
全开硬件与 `GFX_FRONTEND_BIN=0` 并存是正常配置。
把默认后端翻转记成硬件构型变化会走错证据链，两者必须分别断言。

### 7.4 新增文件要做的两件事

1. 文件头加 `` `include "engine_feature.vh" ``，否则构型构建时该文件里的
   `` `ifdef `` 会被当成未定义而静默走关闭分支。
2. 若新文件属于某个可选机制，把文件名加进 `feature_config.py` 的对应源列表，
   否则它在关闭该机制的构型里仍会进编译输入。

### 7.5 生成物与判据

`tools/check/feature_build.py` 生成 `engine_feature.vh`、`engine_config_guard.vh`、
`engine_config_error_modules.vh`、`engine_regs_offsets.vh` 与软件侧 `gfx_feature.h`，
都不手改。`--sync` 写主工程默认构型（`feature_config.CANONICAL`，现为 B0 基本档）及综合模式，`--check` 校验。

判据入口：`validate.py --checks feature-caps feature-elab feature-probe feature-contract feature-function`
（WSL），`feature-synthesis` 与 `feature-board` 走 Windows Efinity，
清单与逐项用法见 `tools/check/README.md`。

主工程默认是 B0 基本档，像素能力全开。全结构构型的综合模式为 speed2，
其它构型为 speed，均由 feature_config.synthesis_mode 生成并登记到 config.json。
不同模式的资源差不能直接作为单机制成本。
历史两轮来自不同构建目录，不能据此断言严格同输入存在随机波动。
报告余量需绑定源码、目录、综合模式、时钟和实际产物；跨构型或跨轮比较保留这些边界。

### 7.6 关闭态与后端切换

无 TEX 时 mover 的九个输入接空闲常量，不能省略端口后让共用读路消费 z。
无 PB 时软复位仍恢复全游戏区 clip；只有 PB 内部中止可以保留 clip。
门铃按当前 CTRL.BIN 选择新请求，pb_active 仅说明在途事务的所有者，不能用它
决定新请求是否进入 STRIP。TEX 能力不存在属于正常裁剪，游戏跳过热集装载；
能力存在但装载或 CRC 失败仍记录错误。
