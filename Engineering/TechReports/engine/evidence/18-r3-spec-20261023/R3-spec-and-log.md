# R3-B7 预设集成与板上对拍

状态:**待验收**（2026-10-23，R3实现与选定acceptance判据全部完成，见§4.4）。原 §4.1 的 13/13
只证明部分静态事实，漏掉构型枚举、集成排空与性能约束，不能据此宣称输入已冻结。
前期研究订正见§4.2；B7已正式实现，过程与完整证据已并入engine第18章。R2 已于 2026-10-23 验收(7856164),
B7 断言值 0xD46F 经研究确证。总览见 三维光栅总览.md（当时Todo索引，现由本章归并）。

## 1 要解决什么

把 R2 的光栅核接进整机:机制开关、预设、寄存器、**顶层页选**、条带 FSM 相位、综合时序与
板上对拍。原则:B0~B6 既有构型行为不变(综合产物按波动带口径对照,不要求逐位——
布局结果本有运行间波动);演示与交付用 B7 位流;压力模式 N_max 做**记录性复测**
(「不追 N_max」定案,不设门槛)。

## 2 怎么切分

### 2.1 生命周期(单一,评审定案收敛)

**全帧只有一条控制通路**:第 1 页只是参数/状态页,**不设**独立的 EN/门铃/DONE。

```
CPU 写参数(第 1 页:TRI_BASE/SUB_BASE/SUB_BYTES/TRI_COUNT/RAS_KEY/TEXCFG)
→ 第 0 页既有 DOORBELL(STRIP 模式)受理,同拍锁存全部 3D 参数(与 LIST_BASE 同纪律)
→ 配置预检、既有 2D 配置段与条带头读取完成，再启动一次 rlst.go
→ engine_cmd 条带 FSM:LOAD 与 CLEAR_Z 并行、分别记住完成 → RAST(子表执行+排空检查)→ 2D 命令 → FLUSH
→ 第 30 带 FLUSH 后撤 ack，等 rlst 末尾校验与全部单元静止 → 既有 list_done IRQ / DONE
错误并入既有 IRQ_STAT/ERROR_INDEX/TAG(ERROR_STAGE=7 光栅)
```

- 旧 2D 完全绕过:**TRI_COUNT=0** 时 FSM 直接跳过 RAST 相位(零开销),不新增 CTRL 位;
  `GFX_HAS_RAST` 关闭的构型该相位不进编译输入(第 7 章关闭态)。
- **3D 入口约束**：`TRI_COUNT>0` 时必须 `CTRL.BAND=1`、`LSTRIPS=30`、
  `STRIP_H=16`，背景/目的表面覆盖 640×480。R2 的子表 ABI 固定 30 段；不能直接与
  旧 1～30 带控制器任意组合。无效配置在取 3D 表或启动 BAND 前报 BAD_FIELD/stage7。
  `TRI_COUNT=0` 不检查闲置 3D 参数，保留旧直投/标量及 1～30 带行为。
- 复位/错误的统一收尾见 §2.6。z RAM 不逐字复位，下一带重新 CLEAR_Z；
  DONE/ERR、BUSY 清零、软复位完成都须保证不能再有旧事务写入。

### 2.2 机制开关与预设(feature_config.py,第 7 章流程)

- `STRUCT_NAMES` 增加 `RAST`、`RAST_TEX`;依赖边:`RAST → STRIP`、`RAST_TEX → RAST`、`RAST_TEX → TEX`。
  后一条是单向依赖，RAST_TEX=0 并不要求 TEX=0；L1 仍可保留旧 2D 纹理能力。
- **非法组合冻结:`PB ∧ RAST` 非法**(首发未定义两者共存,由 is_legal 拒绝;
  以后要做引用执行接入光栅时另立工作点)。`FEATURE_SOURCES` 登记新文件。
- 新预设 **B7 = B3 + RAST + RAST_TEX**(TEX=1,BAND_HW=1,STRIP=1,INST=1,PB=0);
  `CAPS[14]=RAST`、`CAPS[15]=RAST_TEX`,B7 断言值 `0x0000D46F`
  (B3 的 0x146F 按位或 0xC000，保留 INST 的 bit12；原 C46F 在 R2 前期研究中修正)。
  CANONICAL 保持 B0。完整枚举必须保留独立 DELIVERY 位并执行 PB/RAST 互斥：
  **22 个合法结构、176 个 research、44 个 delivery，共 220 个合法完整构型**；
  128 个结构中非法 106 个，2048 个完整输入中非法 1828 个。依赖图原提案的 25/150/50
  漏了互斥且错误覆盖 DELIVERY，禁止采用。生成守卫须含互斥反例，不能只加三条依赖边。
- **synthesis_mode 显式保留旧五位条件**：`all(TEX,BAND_HW,STRIP,INST,PB) OR RAST`
  时用 speed2，否则 speed。不可写成 `PB OR RAST`（会改动旧 160 个完整构型中的 30 个），
  也不可只按 B5/B7 名字选择（还有合法自定义构型和 L1）。B0～B6 与旧全部 160 构型
  逐项等价，B7/L1 用 speed2；未来新构型再按自己的综合证据决定。

### 2.3 寄存器与顶层页选(regs_gen.py 单一真源)

- APB Slave0 **第 1 页**(0xF810_1000)落 3D 参数/状态:`ID`/`CAPS`(镜像 14/15 位)/
  `TRI_BASE`/`SUB_BASE`/`SUB_BYTES`（子表真实逻辑长度，物理分配补齐到 16B）/`TRI_COUNT`/
  `RAS_KEY`(全局色键,ARGB;billboard 弹幕抠底用)/
  `TEXCFG`(v1 布局号 0：64×64 atlas、stride=64 词，含四个 32×32 区块；其它值拒绝)/
  `PERF`(光栅周期数、细条剔除计数等)/`DEBUG`。无门铃/DONE(2.1)。
- **gfx2d_top 一级页选必须扩**(评审证实 `apb_sel_eng` 只放行第 0 页与 4~7 页,第 1 页随
  axi_probe_master 撤下落默认应答):`apb_sel_eng` 增 `paddr[15:12]==4'h1` 支路;
  R3 改动清单明确含此层,engine_top 内分选(第 0 页寄存器/第 1 页 rast/第 4~7 页 TEX)随同。
- 未映射偏移回 PSLVERR 的既有规则沿用;关闭态(无 RAST 构型)第 1 页读写拒绝进
  第 7 章关闭态契约表。
- 页 1 的参数寄存器是 shadow，复位值 0；CPU 可在 BUSY 时准备下一帧，但执行只读门铃快照。
  CSR 保留原始 32 位以便检测，不能先截成 u16/u24 再校验：TRI_COUNT≤65535、
  SUB_BYTES 为偶数且≥120、主表 32B/子表 16B 对齐、主表末端和子表补齐包络均作
  33 位范围检查、TEXCFG=0。TRI_COUNT=0 按 §2.1 绕过。驱动同时检查实际分区容量、
  表面与列表不重叠、缓存发布与完成后读回失效；不能只增加六次 MMIO 写入。
- **表容量按引用数计**：`SUB_BYTES=120+2*Σrefs[strip]`，物理分配上取整至 16B。
  1100 条记录跨满 30 带时子表 66120B（物理 66128B），主表 140800B；
  最多 65535 条记录的满带子表 3932220B。每带 count 为 u16，可重复索引，必须先检查再写。
  没有跨带参数缓存，每个引用都可重新读 128B，带宽预算不能用主表容量代替。
- 3D 帧 BUSY 期间由顶层拒绝 TEX 页 APB 读写（PSLVERR，屏蔽 TEX 的 psel/RAM 使能），
  防止改 atlas 或持续 APB 抢占导致半帧变纹理/光栅饥饿；RAST=0 的旧构型沿用原规则。
  相位租约只控制端口调度，不能代替整帧纹理内容的所有权。

### 2.4 条带 FSM 相位与端口接(engine_cmd / engine_band / engine_tex)

- `LOAD || CLEAR_Z → 两者完成屏障 → RAST(rlst 取子表+rast 执行+排空检查)
  → 2D 命令 → flush`；z 每拍清四像素，实际 RTL 有 2560 个写拍，
  从受理边到 done 边相隔 2561 个时钟（未计帧层衔接）。
  count=0 时读完头部即跳过记录/像素扫描，不宣称头部读取和已启动清零没有成本。
- 只在 `band_start && band_start_ready` 受理时发一次 z_clear 并清 `load_seen/z_seen`；
  两个 done 都是单拍，分别置位并保持到本带结束，不能直接 `band_loaded && z_clear_done`。
  屏障通过且已收到对应 `strip_req` 后拉 ack；检查 `cur_strip==strip_id`。
- engine_band 光栅写口(单像素,valid/ready)在 RAST 相位独占;engine_tex 采样口互斥
  纪律同 R2 §2.4；带 N 的 z-clear 与带 N 的 LOAD 并行，不能在旧 RAST 未排空时提前清零。
- `strip_ack` 是电平，保持到本带 2D/FLUSH 完成；`ras_lease` 只覆盖 RAST。
  第 30 带也必须先撤 ack，等待 `frame_done`、错误尾检和 idle，再完成整帧；
  不可复用旧 S_BWAIT 的“最后一带 flushed 立即 DONE”。子表尾部多字节应 ERR 而非 DONE。
- FD 为旧 CMD/INST 与 RAST 共用一条描述符入口；按**受理时**类别锁存 owner，
  到状态和流末拍都收齐才交接，不按当前相位切返回路由。RAST 的 FD/FS、keep/cidx、
  `rd_stat_raster`、BAND 的 ras_* 和 TEX 的 samp_* 全部显式接线；禁用分支全部接确定空闲值，
  `-Wportbind` 不允许新增悬空输入。TEX_READ_LATENCY 与 rast 的 TEX_LATENCY 必须一致。
- 错误语义:光栅首错停止本带后续三角形,已提交像素不回滚(与 10~12 类总线错误同哲学),
  定位经 ERROR_STAGE=7 + ERROR_INDEX/TAG。

### 2.5 综合与板上

1. 回归按验证流程按影响选择:B0~B6 跑**仿真回归**(实际消费者清单),不重跑七档综合;
   B7 与受影响构型才综合。资源/时序对照用**波动带口径**(36036 波动带先例),
   不要求与历史产物逐位一致。
2. B7 四步综合+时序:目标 `core_clk` setup ≥+0.3ns 且所有时钟非负;不达标先关
   RAST_TEX 保 L1(总览冻结点规则)。
3. 板上对拍:R1 参考光栅器板上构建与硬件同帧 CRC(含注错对照组)。
4. **水位与总线健康**:displayFifoProbe 不再适用(依赖已撤下的 axi_probe_master,
   且其窗口正是划给 3D 的第 1 页)。改用:整机仿真水位(E5 口径)+ 板上 display 既有
   欠载判据(displayTest 通路)+ 引擎 `PERF_WAIT_DDR` 三件,3D+2D 混合负载欠载必须 0。
   必须在**同一 3D+HUD 负载运行期间**采样 display，SRC=1、SNAP_FRAME 持续推进，
   双读场号保证快照一致；预热后清统计，在负载前/中/后读取 ZERO/UNDER/RESPERR，
   均为 0。单跑 displayTest 只能证明观测通路，WAIT_DDR 是引擎反压诊断而非显示欠载证据。
5. **N_max 记录性复测**:固定旋钮、3600 场口径,实测数字入报告与 1891/2237 同表对比;
   **不设验收门槛**(「不追 N_max」定案);若显著回退须日志记录原因(预期:光栅旁路不影响,
   差异来自布局波动)。
6. validate.py 的 feature-caps/elab/probe/contract/function 全过,
   feature-synthesis/feature-board 清单扩入 B7。

### 2.6 统一取消、首错与排空（复核新增）

既有 mover 在 drain 中自行接收并丢弃 FS（`fsr_tvalid` 被屏蔽）；rlst 的 cancel
却要求旧 FS 末拍和状态都到齐。因此不能把二者直接同时 cancel/drain 后等 rlst.busy，
也不能只把 mover.drain_done 接到新软复位完成端。冻结以下集成顺序：

1. 独立于 engine_cmd 软复位清零的收尾状态机保留事务身份。软复位、控制层复位或首错
   当拍禁止新 FD/采样/像素提交，取消 rast/rlst，并启动既有 mover drain。
2. **锁存** mover 的 drain_done 单拍。物理排空确认后对本事务的 rlst 状态作局部复位，
   解除已被 mover 吞流造成的等待；此前不可清 FD owner 或复用其槽位。全局 DDR 复位另按
   平台复位处理，不假设 AXI 会在任意时限内回应。
3. rast 继续退休已发 z/TEX 响应，不用取消完成脉冲推断静止；增加公开 `idle` 输出，
   至少包含 M_IDLE、两槽空、in_flight=0。等待 z_clear_busy=0 和 TEX 未返回计数为 0。
   这些条件与锁存的 mover_done 汇合，才产生一次 frame_quiet/soft_rst_done。
   不可把不同拍的完成脉冲直接相与。
4. rlst.error 是保持电平，rast.err_valid 是单拍；统一锁存**首个** code/index/tag/stage，
   index 零扩展完整 16 位（FFFF 头部哨兵保留），错误同时发生时固定优先级
   `TIMEOUT > rlst > rast`。超时来源按已受理事务的 owner 定位，不按当前相位猜测；
   属于旧 2D/BAND 的错误保留原 stage/语义。
   frame_done 只是取数终止（也可能带 error），取消时它还会被抑制，不能用作错误/取消完成。
5. 3D 首错取消整帧剩余工作，不再执行该带 HUD/FLUSH；先前已 flush 的带不回滚，
   出错的后缓冲不 present。ERR 同样在静止后上报。重启前通过 cancel/idle 的恢复握手
   或事务局部复位清 rast.err_hold，释放 cancel 后确认恢复，再接新门铃。
6. HUD等旧2D命令报错时先保留旧FE/geom的收尾纪律；其预取可能仍在途。
   等旧通路静止才发三维取消/全局排空，不让mover先吞掉旧FE还在等待的FS。

正反例须覆盖：LOAD 早/晚于 z-clear、1/29 带非法入口、末带子表尾错、早读状态+
末拍停顿、mover 吞 FS 的取消、错误同时到达、TEX L=1/2 在途、zc 在途、
软复位完成一次性，以及 ERR 后再次启动有效帧。R2 的单元取消测试不能替代本项。

### 2.7 性能边界与开发顺序（复核新增）

CPU setup + FPGA 增量光栅 + 条带颜色/z + 2D HUD + 连续写回的路线保留。
不为未经测量的演示数量改成硬件 setup，也不把 SIMD/跨带缓存作为正确性首版前置。
但撤销“1100 tri 约 6ms”和“1px/cycle、18% 因而吞吐不构成约束”的结论。
当前核每拍扫描的是 bbox 候选点，包含未覆盖点，且有取数、初始化、credit 停顿与三角形排空。

本轮板上 O2 三次中位数（确定性合成输入、现有 B0，**不是 R4 帧性能**）：

| 输入三角形数 | view→setup | 独立分桶 | 两项合计 |
| --- | --- | --- | --- |
| 16 | 7.345ms | 0.341ms | 7.686ms |
| 32 | 14.687ms | 0.691ms | 15.378ms |
| 64 | 29.360ms | 2.841ms | 32.201ms |
| 1100 | 504.453ms | 48.318ms | 552.771ms |

view→setup 已含裁剪/投影/setup，但不含模型/视图变换、游戏逻辑、提交、FPGA 和显示。
单独 screen polygon setup 的 1100 条也需 223.056ms。分桶按当前两遍实际代码测量，
不能拿 PC 时间推板上预算。1px/cycle 是峰值，不是帧率保证；最坏 1100 个满屏 bbox
就有 337920000 个扫描点，理想扫描也要 3.3792s。

下一轮先按本 spec 做 R3 小规模功能闭环（冻结 CRC/错误/排空/关闭态），再 B7 综合和板测。
R4 从 **16～32 个动态三角形**起测，保持世界深度与 billboard 语义；数量只是起测点，
30fps 仍由真实整帧 ≤33.3ms 判定。1100 条列为压力/优化目标，不能称首版保底。
要恢复该规模，先对 CPU setup/分桶做剖析、缓存不变结果或实现同数值语义的专用快路，
用 R1 向量/参考器证明字节与像素一致，再以板上时间闭合预算；该优化归正式开发轮。
关闭 RAST_TEX 只减少硬件纹理资源/工作，不能自动修复当前通用 CPU setup 的耗时。

## 3 判据

| 判据 | 口径 |
| --- | --- |
| 旧构型行为 | B0~B6 与合法自定义构型按消费者回归；关闭态契约(第 1 页读写拒绝、CAPS[14:15]=0)过 |
| B7 时序 | setup ≥+0.3ns、hold 非负、所有时钟非负 |
| 板上 CRC | ≥11 帧金标准帧软硬件两面 CRC 一致;注错见红 |
| N_max | 记录性复测,数字入报告,不设门槛 |
| 水位/总线 | 整机仿真水位 + display 欠载判据 + PERF_WAIT_DDR,欠载 0 |
| 生命周期 | TRI_COUNT=0 零开销推进在波形可指认;软复位排空 RAST 在途的用例过 |
| 开发前复核 | §2.2 全枚举计数、§2.3 引用容量、§2.4 脉冲屏障、§2.6 取消与末带尾检落实为正式集成用例 |
| 性能声明 | §2.7 只作前期 CPU 采样；真实 3D 帧须另测，不能引用峰值像素率或旧 6ms 预算作验收 |

## 4 开发日志

### 4.4 2026-10-23：实现与验收收口（待用户验收）

R3正式实现完成，生产默认仍B0，Flash未改。当前永久接口在Main引擎手册第8节，
过程、原研究订正、失败反例、判据及数据已归并到
[engine第18章](../../18-B7预设集成与板上对拍.md)。

- 当前R3五构型、35个正常基准帧加协议场景、四类变异PASS；包括两个裁剪构型、
  L1/L2/LAT2，HUD预取、真实事务owner超时、尾错、三处软复位、独立控制复位和计数回绕。
- R2三项PASS；L1/L2/LAT2各十万条、三种记录跨页及十三变异全部通过，原始数量不缩减。
- 当前22合法/106非法结构、220完整构型及九条守卫、六个旧功能变异和工作流增量PASS。
  原18项共享/软件消费者PASS保留其HUD门控修订前边界；未受影响的结果不重复。
- 最终B0=36577 XLR/105 RAM/16 DSP，core +0.827/+0.026ns；
  B7=59623/187/26，core +0.762/+0.026ns，四步及所有时钟关系通过，自写位宽告警0。
- 同BIN旧功能板测B0 12例、B7 14例PASS；最终B7三维11帧三方CRC与padding全对，
  像素注错预期见红，BAD_INDEX三元组/stage7、软复位及恢复正常。
  SRC=1下累计ZERO/UNDER/RESPERR全0、场号推进；E5仲裁/扫描回归PASS。
- 不复位JTAG诊断读到WAIT_DDR=360240、控制器BUSY=3031983、
  最后帧RAST周期39297/退休引用44。WAIT只是反压诊断，不替代欠载证明。
- 记录性HW/TEX N_max=1870（K1632）/1969（K1744），两型最终各3600场零掉帧；
  搜索失败档原样保留，不设N_max门槛，不由差值直接归因。
- 正常三维程序已恢复运行并呈现完整测试帧，本地JTAG复现包在
  `Main/bit/board_release_r3_20261023/`；原有engine_pix、host board.c、报告.work改动保留。

正式轮：R3/R2 `20261023-191846-7ec71037`，当前综合`20261023-191841-995c8477`，
配置/工作流增量`20261023-191846-a722243f`，注册板测`20261023-193434-5007f558`。
板测复用的仅是逐项编译输入相同、字节核对后的综合产物；物理检查实际重新执行。
原始证据一次性归档，见第18章索引。文档收尾仅补路径/引用检查，不重做性能搜索。
本工作点不证明Flash启动、人工HDMI观感或R4完整场景30fps。


### 4.3 2026-10-23：正式开发

范围：配置/寄存器真源、一级页选、engine_cmd 与新增 engine_r3 的单帧控制、
FD/FS 与 BAND/TEX 接线、公开 idle、驱动/板上对拍、集成与消费者验证、B7 综合时序。
开发用 quick；最终 acceptance 包含新增 r3 集成/协议/变异、R2 三项、配置五项及受影响
消费者、B7/基线综合与板测、≥11帧CRC、混合负载显示健康和记录性 N_max。
原有 engine_pix.v、host board.c、报告 .work/ 和上轮研究订正均保留。

实现分工已经落地：engine_cmd 独占帧调度，engine_r3 提供参数页/快照、三维通路和收尾。
配置扩为22结构/220完整构型，B7=D46F且默认仍B0；旧五位配置名兼容。
实际LOAD/z完成是独立脉冲，已做逐带保持；最后一带撤ack后仍等子表尾检。
取消先等mover物理排空，再清可能因吞FS而等待的rlst；清深度、TEX/像素退休另作汇合。
控制层独立复位也挡住本拍像素写，新门铃等旧mover排空后才允许。

初轮真实顶层11帧逐像素通过，字段、索引、记录、尾错、三处软复位和四类变异见效；
补充近处色键/远处覆盖、最后一像素和32位debug回绕场景。每帧退休最多1966050条，
PERF退休快照用21位模差并零扩展32位，实际跨全局32位回绕用例通过。
复核还发现三维子表可能在S_BLOAD阶段取数，原先按RAST相位分类超时不完整；
现由mover锁存的真实DDR读类别产生raster_tmo，rlst提供在途index/tag，
三维超时不再被旧控制器抢成stage0；前置子表和记录超时定向均已通过。

两份早期B7构建/板测只作bring-up：第一份59695 XLR/187 RAM/26 DSP、core
setup/hold +0.444/+0.026ns；11帧板上三方CRC、ZERO/UNDER/RESPERR为0、
BAD_INDEX及软复位恢复、像素注错均已完成。第二份60293 XLR、core +0.721/+0.026ns。
后续owner与计数器修订已触发新构建，不把这些旧位流当成最终源码验收。

当前最终轮：综合`20261023-150221-56e633e1`，R3/R2
`20261023-150238-d1b2e4db`，配置/软件/消费者
`20261023-150246-dee7f54a`；E5仲裁/扫描回归
`20261023-150758-eb9cca53`。早期轮因源码变化失效的结果不复用。
当前配置220构型/六变异、寄存器、编排与软件库已通过，其余仍在跑；
最终状态、完整判据和一次性归档待本轮全部完成后收口。

共享回归最终18项全部PASS并归档到`18-r3-consumers-20261023`；E5仲裁/扫描
已归档。随后补查HUD错误与预取并存：隔离副本在合法AR反压下明确检出
「EARLY LEGACY ABORT」，证明立即R3 drain会抢走旧FE的返回流。修正仅在
RAST已启用的事务内复用既有band_abort收尾条件，非BAND非法配置仍入口结束；
新增永久HUD/预取错误回归，并单独重跑R3与当前构建/板测。旧18项保持其归档源码边界。
另补齐新r3-board/r3-legacy-board的物理检查不自动复用规则。
并行层次注错曾使另一轮输入集合失效，失效轮未用于最终验收；层次注错结束、
源码树恢复后才启动新的R3正式轮。

### 4.2 2026-10-23：前期研究复核与订正（停点=待正式开发）

本轮按用户要求审核前期研究，不开发 B7。原研究脚本在当前源码仍报 13/13 PASS，
但它既未实现集成 FSM，也未模拟消费者的终止/取消行为；部分结果是手写假设的算术自洽，
无法证明架构边界闭合。原始 [18-r3-research](../18-r3-research-20261023)
完整保留；复核证据另存
[18-r3-review](../18-r3-review-20261023)，不覆盖原日志。

**保留的决策**：CPU 几何/setup、FPGA 增量光栅、条带内颜色/z、分相 HUD 与连续写回；
一个门铃/帧控制者、PB 与 RAST 分离、B7 CAPS=D46F，均可保留。
外部一手交叉核对 [RasterIX 设计说明](https://github.com/ToNi3141/RasterIX/blob/main/design.md)
的软件参数准备/增量插值与局部 framebuffer 分工，以及
[Microsoft 光栅规则](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules)
的共享边破平用途，只支持技术分工与规则依据，不证明本机吞吐或 API 兼容性。
本版采样点与定点精度仍以 R1 的自定义契约为准。

**推翻或补齐的结论**（均已改入 §2 和总览/R4）：

| 原研究缺口 | 验证与修订 |
| --- | --- |
| 新结构25、research/delivery=150/50 | 漏 PB∧RAST 互斥的3个结构，并把 DELIVERY 强制设成 ALPHA∧KEY，丢失2种合法 research 输入；完整穷举为22/176/44/220 |
| PB 或 RAST→speed2 也能保留旧策略 | 160 个旧完整构型逐项比较，30 个被该规则改动；改为“原五位全开或 RAST”，全部旧构型等价 |
| 1100 tri 子表约2.3KB，DDR 无压力 | 实际 C setup/bin 构造1100个满屏三角形：旧2340B容量返回-2且输出未变；足量时返回66120B，33000条引用。容量与带宽分开记账 |
| LOAD∧z_done 自然构成屏障 | 两者均为单拍；8×8个到达组合中，直接相与漏56种。每带 sticky seen 后64种均可汇合；zc 写2560拍，受理到done相隔2561周期 |
| mover 排空自然覆盖 RAST | 真实 engine_rlst 反例：模拟 mover drain 吞 FS 而状态已到，cancel后128拍仍 busy；物理排空后局部复位恢复。普通保留FS的cancel可完成。此为协议实验，非 B7 集成验收 |
| 任意1～30带接入、最后flush即完成 | R2固定30带；3D入口限定30，末带必须撤ack并等子表尾检，错误/取消不能等frame_done |
| 错误只扩 mux 即可 | 首错须跨收尾保持，取消两侧、清理rast.err_hold，完整code/index/tag/stage一同上报，ERR后验证可重启 |
| 采样口仲裁等于整帧纹理所有权 | APB对齐写仍可改RAM；新增3D帧busy期间拒绝TEX APB，影子参数与执行快照区分 |
| displayTest/WAIT_DDR代表混合负载安全 | 要在真实3D+HUD负载中观察SRC=1、场号前进、累计ZERO/UNDER/RESPERR，独立通路测试不足 |
| 1100 tri≈6ms、18%扫描负载、cup3d接硬件即满帧 | 板上探针推翻CPU预算；扫描工作按bbox/引用计。R4缩规模先闭合，cup3d报告真实同负载fps，不提前承诺满帧 |

研究订正脚本 [audit.py](../18-r3-review-20261023/audit.py)
与 [取消实验](../18-r3-review-20261023/tb_cancel_contract.v)
覆盖16项核查，结果在
[audit/result.json](../18-r3-review-20261023/audit/result.json)。
复现命令（仓根 WSL，写到新的忽略目录）：

```sh
python3 Engineering/TechReports/engine/evidence/18-r3-review-20261023/audit.py \
  --out Main/sim/build/r3-review-repro
```

板上采样代码为
[setup_probe.c](../18-r3-review-20261023/setup_probe.c)，
[构建入口](../18-r3-review-20261023/build_probe.sh)
调用现有 target.mk/O2 与现有 R1，不实现新快路。固定16/32/64/1100组，每组三次；
各次返回1条记录，输出fail 0/done。中位数见 §2.7，完整串口与部署字节数见
[setup-board.log](../18-r3-review-20261023/setup-board.log)、
[setup-control.log](../18-r3-review-20261023/setup-control.log)，
源码/工具链/BIN/位流身份在
[probe-identity.json](../18-r3-review-20261023/probe-identity.json)。
它是无近面裁剪的确定性合成工作量，polygon组含写入主表，view组只生成记录，bin组单独分桶；
不是完整R4路径，也不是B7吞吐。数据足以否定旧预算，不能外推所有模型的每三角形固定成本。

本轮按仓库验证流程选 **quick**：`rlst/rast-ports` 两项通过（实际取数边界、RAW、取消、
TEX L=1/2连续采样和关闭态）；`board` 通过（重新部署既有B0、两次PC调试、
重建featureTest，同BIN12例CRC/错误/关闭态判据通过，CAPS=040F）。分别归档
[unit-checks/result.json](../18-r3-review-20261023/unit-checks/result.json)、
[board-smoke/result.json](../18-r3-review-20261023/board-smoke/result.json)。
这些只确认已有消费者与当前B0基线，不是三维整机/B7/性能验收。

保留原有 engine_pix.v、host board.c 和报告 .work/ 改动；生产配置、寄存器、RTL和软件均未修改。
下一轮从 §2.2/§2.3 真源与关闭态开始，再落实 §2.4/§2.6 的单帧协议，
增加正式集成反例，然后综合B7和做软硬CRC板测。性能按 §2.7 分项测量后再扩场景。

### 4.1 2026-10-23：前期研究与集成边界核实（停点=待实现）

**历史记录，以下“无新矛盾/已冻结”及 25/150/50、2.3KB 容量判断已由 §4.2 推翻；
原脚本与原始日志保持不变，以便对照。当前开发输入以 §2 订正契约为准。**

接续 R2 验收（7856164）。本轮只做研究与事实核实，未写生产 RTL、未改寄存器与构型；
`engine_pix.v`、host `board.c` 与报告 `.work/` 为原有未提交内容，照旧保留。
研究原始材料一次收录到
[18-r3-research-20261023](../18-r3-research-20261023)：
[research.py](../18-r3-research-20261023/research.py)、
[research.json](../18-r3-research-20261023/research.json)、
[research.log](../18-r3-research-20261023/research.log)
与两份板上基线日志。复现算例（从仓根，输出换到新的忽略目录）：

```sh
python3 Engineering/TechReports/engine/evidence/18-r3-research-20261023/research.py \
  --out Main/sim/build/r3-research-repro/research.json
```

板上基线：JTAG 识别 Ti60（idcode 0x10660a79），调试通路连续两次会话读 PC OK
（当前部署位流=R2 验收时从工作区重建的 B0，本位流功能已经
featureTest 12 例 CRC 与生产 240 帧验证）。三维板测本身按 §2.5 属实现后的验收轮。

13 项可执行判据全过（每项先正则核实真实源码，再导入 feature_config/regs_gen 计算）：

1. **页选**：`gfx2d_top` 的 `apb_sel_eng=(paddr[15:12]==0)||(paddr[15:14]==2'b01)`，
   第 1 页现落默认应答（PREADY=1/PSLVERR=1），TEX 占页 4~7——证实 spec 的判断。
   R3 改动=gfx2d_top 一行扩 `4'h1` 支路；`engine_top` 增 `apb_hit_rast` 与三路 mux
   （现 mux 是 `apb_hit_regs ? regs : tex` 二路，不能让页 1 落进 TEX 分支）。
   关闭态按 §2.3：无 RAST 构型页 1 由 engine_top 内常错应答从动拒绝，整块不进编译输入时
   gfx2d_top 的放行支路仍指向 engine（页 1 不回归默认应答，契约表增行）。
2. **FSM 插入点**：S_BSTART/S_BLOAD/S_BFLUSH/S_BWAIT 状态直译 band_start/flush；
   空表先例（S_BLOAD 出口 `count_r==0` 直接 FLUSH）是 TRI_COUNT=0 零开销推进的现成模式；
   条带表头 8 拍、条带号 1..30、表步长 32KB。RAST 相插在 S_BLOAD 出口与 2D 派发之间。
3. **rlst 握手映射**（读 RTL 语义订正）：rlst 逐带发 `strip_req/cur_strip`（count=0 的带
   也发，只花头部成本）；帧层备好后拉**电平** `strip_ack`；rlst 排空后发 `strip_done`；
   帧层保持 ack 至本带排空、2D/FLUSH 完成才撤下；rlst 见撤下才请求下一带；30 带后
   `frame_done`。FSM 侧对应：S_BSTART 同拍起 `z_clear` → S_BLOAD 等
   `band_loaded ∧ z_clear_done`（双完成屏障）→ 拉 strip_ack 并进新状态 S_RAST 等
   `strip_done` → 撤 ras_lease → 既有 2D 派发/S_BFLUSH/S_BWAIT → 撤 strip_ack 回 S_BNEXT。
   ras_lease 只覆盖 RAST 相；strip_ack 覆盖更长，两者不能混用。
4. **门铃锁存**：`kick=door_accepted` 一拍，cmd 在 S_IDLE 同拍锁存 base/count/strips/
   table_base。3D 六参数（TRI_BASE/SUB_BASE/SUB_BYTES/TRI_COUNT/RAS_KEY/TEXCFG）须同一拍
   锁存；页 1 CSR 只作 CPU 可见的存放处，不设独立受理。
5. **错误通路**：BAD_INDEX=13 已在 regs_gen 登记，4 位码剩 14/15；PB 现用 stage={0,1,3}，
   **7 空闲**；ERROR_TAG 位形 11+5+16=32 合法。engine_top 错误 mux 在
   `err_code/err_stage` 两行扩 rast 分支，**ERROR_INDEX 也须 mux**（rlst 输出 16b 完整
   主表索引，头部错误 FFFF 哨兵；现 mux 只有 code/stage 两路）。
6. **CAPS 与依赖图**：B3=0x146F，既有预设 bit14/15 全 0，**B7=0xD46F 确证**；旧预设值
   不因新增位改变。7 结构位合法组合**25** 个（全配置 research/delivery=**150/50，共 200**；
   旧 16/128/32/160）——`_self_check` 与 feature-elab/contract/function 的穷举计数按此
   实算更新，不许改断言凑数；GUARDS 增 3 条命名守卫（RAST 需要 STRIP；RAST_TEX 需要
   RAST、TEX）。
7. **synthesis_mode**：现行 `all(STRUCT_NAMES)` 对 B0~B6 只有 B5 是 speed2；显式表
   {B5,B7}→speed2 对旧预设逐项等价，是纯扩展，无回归风险。
8. **z 清零**：rast 的 `z_clear/z_clear_busy/z_clear_done` 口在；预算 2560 拍/带×30 带
   =76800 拍=768.0µs@100MHz。z 清零与 LOAD 无共享端口（清 z RAM 是 rast 内部，
   LOAD 走 mover AXI 写 BAND），并行安全；§2.1 与 §2.4 两句合并口径：
   **带 N 的 LOAD 与 z-clear 并行，双完成屏障后进 RAST**。
9. **BAND/TEX 新口**：`ras_wr_ready=租约∧EXEC∧界内∧无保持` 已在 band 实现，帧层只须
   RAST 相拉 ras_lease；tex 采样口与 legacy/APB 互斥已实现（samp_ready 四条件与信用）。
10. **对拍通路**：rastTest 板上入口显式留有「由 R3 承接」接口（软渲染→CRC 已通）；
    驱动经 `gfx_engine.c` 写 LIST_BASE/DOORBELL（提交函数已带 32KB 对齐/条带数/33 位
    范围校验，R3 扩页 1 参数写入与同纪律校验）；CRC golden 机制复用 engineTest 先例；
    displayTest 欠载判据（D_UNDER_BEATS=0）+整机仿真水位（E5 口径）+PERF_WAIT_DDR
    三件套事实在；R1 冻结 16 条记录（`Main/sim/engine/rast/r1-records.json`）可作对拍
    种子。帧容量：目标场景 ~1100 tri 主表 140.8KB、子表 2.3KB，DDR 无压力。
11. **页 1 寄存器偏移提案**（实现时登记进 regs_gen 单一真源，此处仅为冻结草案）：
    `R_ID 0x00 / R_CAPS 0x04（镜像主 CAPS 的 14/15 位）/ TRI_BASE 0x08 / SUB_BASE 0x0C /
    SUB_BYTES 0x10 / TRI_COUNT 0x14 / RAS_KEY 0x18 / TEXCFG 0x1C / PERF0 0x40 /
    DEBUG 0x80`；未映射偏移 PSLVERR 沿用。TEXCFG v1=0 拒绝其它值，64×64 atlas 边界
    (h-1)·stride+w=4096 恰过 rast 的 ≤4096 检查（slog2=6）。

spec 无新矛盾；§2.2 依赖边、§2.3 页选与关闭态、§2.1/§2.4 合并后的 z-clear 时序均按
上述事实落实。文档验证选 quick 的 paths（见本轮收尾）。下一轮从 §2.2/§2.3 的真源改动
（feature_config/regs_gen/feature_build + engine_feature 生成物）开始，先消费者后整机。

- 2026-10-22 spec 成稿,未开工。
- 2026-10-22 静态评审修订:生命周期单一化(废除第 1 页独立门铃/DONE,FSM 唯一,TRI_COUNT=0
  即绕过,为真:原方案两套控制并存无协同定义);gfx2d_top 一级页选补进范围(为真:
  第 1 页现落默认应答);PB∧RAST 定非法;synthesis_mode 改显式表(为真:all(STRUCT_NAMES)
  规则被 RAST 打破);L1/L2 拆 RAST/RAST_TEX 双宏;回归改按影响选择、对照改波动带口径
  (为真:逐位一致不是布局属性);displayFifoProbe 换三件套(为真:依赖已撤下的探针且占第 1 页);
  N_max 改记录性(与「不追 N_max」定案对齐);B3 预算改归档 43816 XLR/165 RAM/19 DSP。
- 2026-10-22 二次修订(弹幕升世界空间):第 1 页增 RAS_KEY 寄存器,门铃锁存清单含之。
- 2026-10-22 R2 前期研究联动：修正 B7 CAPS 为 D46F、补齐 RAST_TEX→TEX 单向依赖；
  增 SUB_BYTES 与物理末拍 padding；四材质定为 64×64 atlas；LOAD/z-clear 改双完成屏障，
  局部空表不再称总零开销。均为规格更新，生产寄存器/构型/FSM 尚未实现。
