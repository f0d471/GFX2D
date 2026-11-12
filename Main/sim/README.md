# sim：仿真

R4软件场景与真实板测的入口为`validate.py --checks r4`和Windows的`r4-board`，
范围、功能/性能分离与日志字段见[三维演示验证](engine/r4/README.md)。

用 WSL 里的 iverilog 跑；含 `Axi_Mux` 的套件需要 Efinity 的原语模型。
旧模块的子目录以 `run.sh` 运行，Python 套件使用表中对应的 `validate.py` 检查名：

```
wsl -e bash -c 'bash sim/<目录>/run.sh'
```

## 目录

| 目录 | 被测模块 | 用例 |
| --- | --- | --- |
| `host/` | PC画布与SDL窗口 | `pc-wide`：解析覆盖/深度、哨兵/原子拒绝、投影比例、世界状态、64/32与变异；Windows `pc-window`：实际窗口/双屏全屏、过滤/纹理恢复、失焦、模拟断屏恢复 |
| `engine/rlst/`、`engine/rast/` | 三维取数、光栅、BAND/TEX 新口 | `validate.py --checks rlst rast-ports rast-rtl`；真实 mover/AXI、L1/L2/L2二拍、两份 C 参考器、协议/取消/十三类注错；acceptance 使用十万条记录 |
| `engine/r3/` | B7真实顶层与单一帧控制 | `--checks r3`：五构型整帧、影子参数、完成屏障、错误/取消/重启和四变异；Windows的`r3-synthesis/r3-board/r3-legacy-board`分别验证综合与板级边界 |
| `hdmi/` | `rtl/hdmi/hdmi_tx.v` 整条通路 | 正常 + 2 个注错 |
| `apb_probe/` | `rtl/bus/apb_probe.v` | 正常 + 2 个注错 |
| `axi_probe/` | `rtl/bus/axi_probe_master.v` 与 `rtl/vendor/axi_mux/Axi_Mux.v`，两个主机并发 | 正常 + 4 个注错。需要 `EFINITY_HOME` 指向 Efinity 安装目录（取 `EFX_SRL8` 原语模型） |
| `axi_mux_guard/` | `rtl/bus/axi_mux_guard.v` 与 `Axi_Mux`（X3，三个主机：扫描替身、引擎替身、CPU 替身） | 读、写两个方向各一组：14 个用例（11 正常/对照 + 3 注错），加两项保护开关逐项比对与一项「inject_wlast 的 FAIL 来自 wlast_err」检查，共 17 项。同样需要 `EFINITY_HOME` |
| `display/` | `rtl/display/` 的扫描输出：`display_fifo`（厂商异步 FIFO 的封装）与 `display_top` | 两个测试台。`tb_fifo_semantics`：正常 + `inject_no_reset`；`tb_display`：`image`（含像素时钟 ±0.5% 两组）、`swap`、`inflight`、`level`、`starve`、`snapshot`、`corerst`、`image_full`（整帧 640×480，用 `FULL=1` 打开）与 7 个注错。只有明文 RTL，不需要 `EFINITY_HOME` |
| `engine/blend/` | `rtl/engine/core/engine_blend.v`（单通道混合核） | `exhaustive`（六个方程全定义域 50462976 个向量，方程用不到的输入跟着变）、`golden`（抽样 1024 条对 `check_round255.c` 导出的金标准）、`stall`（随机插入 `en` 拉低的停顿拍）与 5 个注错。全量一次约 6 分钟。只有明文 RTL，不需要 `EFINITY_HOME` |
| `engine/mover/` | `rtl/engine/core/engine_mover.v`（搬运层）经仓内 `Axi_Mux`（S1）接从机 | `normal`（读侧仲裁、读流分发与上下文、写掩码、状态里的命令索引与末行标记、在途契约、地址与突发约束）、`drain`（读、写突发中途各排空一次，控制层复位与软复位各一组；请求完成后继续举着时 `drain_done` 只打一次、请求撤下之前不收新提交）、`err`（读回、写响应 SLVERR 各一例）、`timeout`（从机按住超过门限）与 5 个注错，另加从机定时模式的回归 `slave_timed`（`tb_slave_timed.v`，直连从机、不经过被测）。需要 `EFINITY_HOME`（`Axi_Mux` 例化 `EFX_SRL8`） |
| `engine/geom/` | `rtl/engine/core/engine_geom.v`（几何层）。同目录另有前期研究的探针 `probe_geom.v`（`probe_geom_intersect`、`probe_geom_d7`、`probe_geom_rowiter`），保留为回归 | 期望值全部由独立模型 `tools/check/geom_model.py` 现算，测试台只做「驱动 + 比对」：定向用例 34 条、159 条向量里几何层见到的 59 条命令、十一整帧用例的 17 条命令，合计 1020 + 338 + 5224 条记录逐字段比、每个描述符过 16 字节对齐/不跨 4KB 的规矩、读写避让的读源/读目的有无、零面积命令的完成脉冲与命令索引；握手监视器查多发的描述符与非空命令的 `cmd_done`，行缓冲替身查每一行都开在空着的那一半上。另加 `+STALL`（三个方向各压反压）与 `+RFSTALL`（行缓冲替身的排空拉长到 23 拍），判据是序列与恒 ready 时相同。五个注错，探针 14/14，全套 27/27。不需要 `EFINITY_HOME` |
| `engine/pix/` | `rtl/engine/core/engine_pix.v`（计算阵列与片上缓存）。同目录另有前期研究的探针 `probe_pix.v`，保留为回归 | 一行一行的单元测试 418 行 / 10216 个槽与五个注错；乒乓测试台 874 行 / 28340 个槽（行与行之间不等，三种写流下游）与两个注错；从几何层到搬运层的真实通路（几何层、计算阵列、搬运层、两个 DMA 核、随机反压从机）30 条命令 512 KiB 逐字对软件执行器；跨命令写后读一组（等写回必须对、不等必须红）；研究探针 9 项，全套 25/25。不需要 `EFINITY_HOME` |
| `engine/ctrl/` | 控制层：研究探针 `probe_check.v`（保留为回归）+ 正式单元仿真 `tb_engine_top.v`（`rtl/engine/engine_top.v` 全链经 APB 提交，从机是 `axi_ram_model`）| 探针：定向 54 条 × 三结构全符、随机 3000 组互比、预计算位等价。正式：159 条一致性向量端到端逐条对错误三元组与内存、定向判据（取数边界/总线错误/超时/软复位/写后读/首错顺序/PERF 与 APB 对账），6576 项检查 0 错；两个注错构建（对调 space/format、关掉逐条回退）各自见红。不需要 `EFINITY_HOME` |
| `engine/frame/` | 整帧仿真：`tb_engine_frame.v`（`engine_top` 经 APB 提交整张命令表，从机 `axi_ram_model` 带随机反压，4 MiB 存储，其中用例窗口由各用例 `cnt.hex` 指定）| 十一个整帧金标准（内存逐 128 位对软件执行器、帧缓冲 CRC 对 `golden.txt` 11/11）+ 基础 Demo 场景 A 连续两帧（`tools/check/sw6_frames_dump.c` 从游戏本体的 `sw6_scene_a.c` 组表导出）+ 定时从机弹幕性能（16 颗 16×16 ALPHA，`PERF_BUSY` 逐颗对账）+ 从机 SLVERR 窗口注错；计数器对账 `PIXEL == pixel_total − src_oob`、`RD/WR_BEAT`、`RD/WR_BURST` 对从机观测值。不需要 `EFINITY_HOME` |

`engine/band/` 是条带缓冲的正式数据面验证，默认 `run.sh` 共 17 项：9 正常、8 定向变异。
真实全部数据面核对软件金标准，含状态/取消/超时/重试、空条带和 8/16/32 行参数。
研究探针仍由 `python3 Main/tools/check/g2_research.py` 独立复现。主帧级 BAND 已启用，完整控制验证见 engine/stripctl。
端口、资源、周期与边界见 [band/README](engine/band/README.md)。

| `engine/stripctl/` | 完整条带控制、生产分桶和软件执行器 | 两套各 10000 张、软件错误边界、16/32 行完整帧、FETCH/总线/排空/复位、4 软件与 3 RTL 变异；入口 `tools/check/check_g3_stripctl.py`，证据见目录 README |

## 规则

检查范围、档位与证据复用统一见 [验证流程](../doc/验证流程.md)，唯一清单由
`python3 tools/check/validate.py --list` 给出。正常／变异判据保留，日常按模块与消费者选择。

`engine/tex/` 为正式 TEX（片上纹理）验证：APB 全窗口、两种读延迟、TEX/DDR 请求重叠
与流隔离、PERF、错误、复位及四个定向变异，共 6/6。研究探针另行保留，
入口和证据见 [tex/README](engine/tex/README.md)。

每个 `run.sh` 除了正常用例，还必须带注错用例：把一处改错，对应的判据必须报 FAIL。全部用例都符合预期才输出 `SIM OK`，否则退出码非零。只跑正常用例无法说明判据真的在检查东西。

注错的机制、命名与提交前检查见 [`../doc/开发环境.md`](../doc/开发环境.md) 0.20 节（原型是变异测试 / functional qualification，与安全标准的 fault injection 不同）。要点：生产 RTL 里不放运行时注错开关，变异分支包在 `` `ifdef INJ_xxx `` 内，开关由 `run.sh` 以 `-DINJ_xxx` 传入，不定义时那些行进不了编译输入。`sim/` 下不综合的研究探针（`probe_*.v`）例外，它们继续用参数化注错口，不进网表。

| 用例 | 注错内容 | 期望 |
| --- | --- | --- |
| `hdmi/inject_hbp` | 行后沿少 1 个像素 | FAIL |
| `hdmi/inject_noinv` | 不撤销 P/N 取反 | FAIL |
| `apb_probe/inject_addr_trunc` | 只把地址低 4 位送进被测模块 | FAIL |
| `apb_probe/inject_data_trunc` | 只把写数据最低位送进被测模块 | FAIL |
| `axi_probe/inject_flip` | 从机把一拍数据翻 1 位 | FAIL |
| `axi_probe/inject_hang` | 从机不发写响应 | FAIL |
| `axi_probe/inject_maxlen` | 从机突发上限 16 拍 | FAIL |
| `axi_probe/inject_timeout` | 超时阈值 1 个周期 | FAIL |
| `axi_mux_guard/inject_noguard` | 关掉 SoC 防饿死保护（`GUARD_EN=0`），激进读引擎 | FAIL，最大地址等待超过同一阈值（实测 889 > 700） |
| `axi_mux_guard/inject_mask_s0` | 让保护掩码把 S0 位也清掉 | FAIL，扫描拿不到授权 |
| `axi_mux_guard/inject_wlast` | 引擎替身的 WLAST 打一拍（2026-09-19 之前的真实 bug） | FAIL，从机 `wlast_err` 非零 |
| `engine/blend/inject_wrong_eq` | 混合核的 CONST 行代入写错（`base`、`m1` 都取错） | FAIL，第一处不符的向量在 CONST 上 |
| `engine/blend/inject_trunc16` | 中间量 `t` 模截断到 16 位 | FAIL，ADD 上 `src + dst ≥ 257` 处丢进位 |
| `engine/blend/inject_unreg_c` | 乘之后只寄存乘积，`base × 255` 仍取当前输入（2026-09-29 草案里真实出现过的错） | FAIL，ALPHA 上两拍的操作数被相加 |
| `engine/blend/inject_copy_dst` | COPY 的 `base` 误取 `src + dst`（抄了 ADD 行） | FAIL，COPY 的第一个向量；方程用不到的输入若固定成 0 就看不出 |
| `engine/blend/inject_x_no_en` | 级 2 的 `r_x` 不看 `en` | FAIL，第一次停顿之后；`en` 恒为 1 时看不出 |
| `engine/mover/inject_nogate_rd` | 去掉读方向的在途门控（`-DINJ_NO_GATE_RD`），读核一 ready 就放下一个 | FAIL，读流上下文错位（拍子带的目标缓冲号与命令索引属于另一笔），总线侧也有违例周期 |
| `engine/mover/inject_mask` | 带内掩码旁路（`-DINJ_MASK_BYPASS`），`wstrb` 与 X 字节直通 | FAIL，被标记「不写」的像素被写进了目的区 |
| `engine/mover/inject_nodrain` | 排空旁路（`-DINJ_DRAIN_BYPASS`），排空期不帮忙收读流、也不补哑拍 | FAIL，两个方向都停在忙态，排空完成等不到 |
| `engine/mover/inject_nogate_wr` | 去掉写方向的在途门控（`-DINJ_NO_GATE_WR`） | FAIL，上一笔写还没结束就接受了下一个写描述符 |
| `engine/mover/inject_drain_level` | 排空按电平重进（`-DINJ_DRAIN_LEVEL`），且只在排空态里挡提交 | FAIL，请求举着时 `drain_done` 打了两次，退出排空那一拍接受了新的读描述符 |
| `engine/mover/inject_no_tlast` | 正常写流不打 `tlast`（`-DINJ_NO_TLAST`，早期版本靠计算阵列给而它没给） | FAIL，写核进 `DROP_DATA`，吞掉下一笔的写流 |
| `engine/geom/inject_nosrc` | `-DINJ_SKIP_SURF`：求交跳过第五项（源表面） | FAIL，逐条比对见红：矩形变大，边带、读描述符、写描述符都不同 |
| `engine/geom/inject_no4k` | `-DINJ_NO_SPLIT`：跨 4KB 不拆，一个描述符盖过页边界 | FAIL，`RULE1 ... crosses 4KB` |
| `engine/geom/inject_alpha` | `-DINJ_ALPHA_AS_COPY`：ALPHA 也按 COPY 走「不读目的」的避让 | FAIL，`sideband read-dst` 判定不一致，读描述符少一半 |
| `engine/geom/inject_extra` | `-DINJ_COPY_READ`：COPY 也发读目的描述符（边带仍说不读） | FAIL，握手监视器的 `PROTO handshake count`（消费循环看不见多发的描述符） |
| `engine/geom/inject_norf` | `-DINJ_ROW_NOFREE`：行开始之前不看 `row_free`，配 `+RFSTALL` | FAIL，行缓冲替身的 `BUFERR ... started on busy half` |
| `engine/pix` 的五个注错 | `-DINJ_ROT_OFF`、`-DINJ_SKIP_KEY`、`-DINJ_DROP_MASK`、`-DINJ_EN_NOTREADY`、`-DINJ_CORE_NOEN` | FAIL，各自红在对应的像素比对或协议检查上；`-DINJ_EN_NOTREADY`/`-DINJ_CORE_NOEN` 在恒 ready 的测试台上必须不红 |
| `engine/tex` 的四个注错 | `-DINJ_VALID_EARLY`、`-DINJ_WR_LOOSE`、`-DINJ_STAT_DROP`、`-DINJ_TX_NOWAIT` | FAIL，证据行分别是 `STR-DATA`、`APB-UNALIGN-SIDE-EFFECT`、`TEX INJECT 4 STATUS_MISSING`、`PIXEL_STREAM_COLLISION` |
| `display` 的七个注错 | `-DINJ_NO_DRAIN`、`-DINJ_NO_CLEAR`、`-DINJ_SWAP_NOW`、`-DINJ_INFLIGHT2`、`-DINJ_LAG0`、`-DINJ_PHASE3`、`-DINJ_ORDER_REV` | FAIL，各自红在对应的判据上 |
| `engine/geom` 的探针注错 | `probe_geom.v` 的 `INJECT`（跨 4KB 不拆、漏第五项、`probe_geom_d7` 的 ALPHA 误避让） | FAIL，各自红在探针自己的判据上（14/14 → 见红） |

后两个 `apb_probe` 注错对应的是顶层曾经真实存在的 bug：APB 信号在顶层没有声明，被当成 1 位线，32 位地址和数据都被截断。

`axi_mux_guard` 的其余用例不是注错：`grant_sequence` 复现仲裁的固定授权序列；
`dual_direction` 验两个方向的保护不会互相提前解除，并检查同一拍两个方向都触发时 `GUARD_HITS` 加 2、
两个方向都还没握手时 `S2_MAX_WAIT_AR/AW` 已经在涨（guard VERSION 2 每拍更新；版本 1 在这一项读到 0）；
`normal` 与 `inflight1` 分别验读方向上保护开启与只靠措施一的口径；`inject_midgrant` 验 guard 不伪造抢占；
写方向 `wr_aggr_guard`/`wr_aggr_noguard`/`wr_contract` 在原从机上、`wr_q4_*` 三例在写队列 4 的从机上各跑一组，
全部期望 PASS，并由 `run.sh` 比对保护开、关两例的统计必须逐项相同（写方向上 guard 没有可观测的作用，
写方向的等待靠措施一）。全部全链路用例都要求从机的 `wlast_err` 为 0。

## 范围

DDR3 控制器与官方的 `axi_interconnect_beta` 是加密 RTL，SoC 网表里也有两个 CPU 运算模块（`EfxCPUSp1`、`EfxCPUSp2`）加密，开源工具读不了，所以整机仿不了。能仿的只有明文模块；第三方的 `Axi_Mux` 是明文，但例化了厂商原语 `EFX_SRL8`，要带 Efinity 的原语仿真模型。
扫描输出的异步 FIFO 用的是 `rtl/vendor/ddr3_controller/common/efx_fifo_v2.3/efx_fifo_wrapper.v`：厂商文件，但是明文、不例化任何原语，iverilog 直接能编，所以 `display/` 不需要 `EFINITY_HOME`。
引擎开发阶段要自己写 AXI 从机模型，并且必须带随机反压和可配的响应延迟，测试台里的从机比真从机强等于没验。

`axi_ram_model.v` 是 `axi_probe`、`axi_mux_guard`、`display` 与 `engine/mover` 共用的从机，
它的 `ADDR_STALL_MAX`、`RD_QUEUE`、`WR_QUEUE`（读、写地址队列深度）与 `hold_ar`/`hold_aw`
都带默认值，默认值保持原来只有随机反压的行为不变；
改它之后用到它的仿真都要复跑（2026-09-19 加 `WR_QUEUE` 与 `wlast_err` 之后，`axi_probe` 五个用例的日志与改前逐字节相同；
2026-09-30 搬运层补四样之后，`axi_probe`、`axi_mux_guard`、`display` 三个套件的完整输出与改前逐字节相同；
同日复核修了定时模式的三处，只动 `TIMED=1` 的分支，三个套件重跑仍逐字节相同）。

2026-09-30 搬运层补的四样，全部是新增参数、默认值保持原行为：

| 参数 | 默认 | 作用 |
| --- | --- | --- |
| `MEM_WORDS` | 4096 | 存储字数，地址取模改用它；整帧用例的设备内存 4 MiB 时取 262144 |
| `STRB_EN` | 0 | 0 按整字写（原行为）；1 按 `wstrb` 逐字节写。`wstrb` 是新增端口，默认不读它，所以既有测试台不接这个端口也保持原行为 |
| `ERR_LO`/`ERR_HI` | `0xFFFF_FFFF`/`0` | 落在这个字节区间的突发回 `SLVERR`，默认值让判断恒为假 |
| `TIMED` | 0 | 0 随机反压（原行为）；1 定时模式，用 `RD_LAT=23 RD_BUB=12 WR_BUB=6 B_LAT=1`（板上实测校准值）：第一个 R 握手在 AR 握手之后第 `RD_LAT+2` 拍，W 通道每收满 `WR_BUB` 拍空一拍，最后一拍之后 `B_LAT` 拍出写响应。与前期研究校准用的从机逐拍相同，回归是 `engine/mover/tb_slave_timed.v` |

从机发现 WLAST 与突发长度对不上时打印 `MODEL: wlast mismatch` 并把 `wlast_err` 加一，
用到它的测试台必须把 `wlast_err` 非零判为失败：只打印不计错，会让一个把写通路弄死锁的主机替身看起来像「CPU 被饿死」。

产物在各子目录的 `build/` 下，不进版本库。

整机条带接入的真实游戏整帧与同板证据在 [engine/g4](engine/g4/README.md)：
check_g4_integration.py --rtl 使用 8 MiB 随机反压模型，检查 TEX/BAND、
整帧可见区与 padding、DDR/PERF，并要求三种变异见红。

绘制前端的方案研究在 [engine/frontend](engine/frontend/README.md)：
1680 份真实状态的阶段记录计数与生产构造观察点等价，以及既有整机 CSV 的分型回归。
实例与硬件保序分桶已完成 RTL、板测与验收，入口与证据范围见该目录 README；
新运行默认写入忽略的 `sim/build/check-runs/`，历史 logs 保留其版本边界。

凝时的 `sh` 检查包含真实场景的范围回填/下一帧命中、相机/投影/视口失效和原子发布，
并直接编译目标端生产流水，以延迟引擎/显示模型验证 A/B 记录与子表、三态提交、
在途稳定性、呈现顺序、输入代际和错误排空；遗漏回填、错误基址、覆盖在途表、提前呈现、
跳过排空的变异均须见红。`sh-board` 在冻结 B7 上比较八帧不同输入代的生产流水/同步硬件/
板上软件参考，以及贴墙开火装填；性能构建另跑双 1800 次实际呈现，SHLAG 延迟独立登记。

几何优化额外对照2048组阴影环及96组近相机/宽屏/非标准投影场景，并检查20万组完整u32商余数、
10万有符号梯度、5万投影及O3/O2/Os、原除法、原阴影路径的记录一致性。
阴影接缝、复用视坐标深度和整数余数校正的变异必须见红。
