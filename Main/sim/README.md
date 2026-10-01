# sim：仿真

用 WSL 里的 iverilog 跑，不依赖 Efinity。每个子目录一个 `run.sh`，进去直接运行：

```
wsl -e bash -c 'bash sim/<目录>/run.sh'
```

## 目录

| 目录 | 被测模块 | 用例 |
| --- | --- | --- |
| `hdmi/` | `rtl/hdmi/hdmi_tx.v` 整条通路 | 正常 + 2 个注错 |
| `apb_probe/` | `rtl/bus/apb_probe.v` | 正常 + 2 个注错 |
| `axi_probe/` | `rtl/bus/axi_probe_master.v` 与 `rtl/vendor/axi_mux/Axi_Mux.v`，两个主机并发 | 正常 + 4 个注错。需要 `EFINITY_HOME` 指向 Efinity 安装目录（取 `EFX_SRL8` 原语模型） |
| `axi_mux_guard/` | `rtl/bus/axi_mux_guard.v` 与 `Axi_Mux`（X3，三个主机：扫描替身、引擎替身、CPU 替身） | 读、写两个方向各一组：14 个用例（11 正常/对照 + 3 注错），加两项保护开关逐项比对与一项「inject_wlast 的 FAIL 来自 wlast_err」检查，共 17 项，实验 E5 2.2 节。同样需要 `EFINITY_HOME` |
| `display/` | `rtl/display/` 的扫描输出：`display_fifo`（厂商异步 FIFO 的封装）与 `display_top` | 两个测试台。`tb_fifo_semantics`：正常 + `inject_no_reset`；`tb_display`：`image`（含像素时钟 ±0.5% 两组）、`swap`、`inflight`、`level`、`starve`、`snapshot`、`corerst`、`image_full`（整帧 640×480，用 `FULL=1` 打开）与 7 个注错。见 `../../Engineering/Todo/hw/扫描输出.md` 2.3 节。只有明文 RTL，不需要 `EFINITY_HOME` |
| `engine/blend/` | `rtl/engine/engine_blend.v`（引擎 P1 B1 的单通道混合核） | `exhaustive`（六个方程全定义域 50462976 个向量，方程用不到的输入跟着变）、`golden`（抽样 1024 条对 `check_round255.c` 导出的金标准）、`stall`（随机插入 `en` 拉低的停顿拍）与 5 个注错。全量一次约 6 分钟。只有明文 RTL，不需要 `EFINITY_HOME` |
| `engine/mover/` | `rtl/engine/engine_mover.v`（引擎 P1 B2 的搬运层）经仓内 `Axi_Mux`（S1）接从机 | `normal`（读侧仲裁、读流分发与上下文、写掩码、状态里的命令索引与末行标记、在途契约、地址与突发约束）、`drain`（读、写突发中途各排空一次，控制层复位与软复位各一组；请求完成后继续举着时 `drain_done` 只打一次、请求撤下之前不收新提交）、`err`（读回、写响应 SLVERR 各一例）、`timeout`（从机按住超过门限）与 5 个注错，另加从机定时模式的回归 `slave_timed`（`tb_slave_timed.v`，直连从机、不经过被测）。需要 `EFINITY_HOME`（`Axi_Mux` 例化 `EFX_SRL8`） |
| `engine/geom/` | `rtl/engine/engine_geom.v`（引擎 P1 B3 的几何层）。同目录另有 B3 前期研究的探针 `probe_geom.v`（`probe_geom_intersect`、`probe_geom_d7`、`probe_geom_rowiter`），保留为回归 | 期望值全部由独立模型 `tools/check/geom_model.py` 现算，测试台只做「驱动 + 比对」：定向用例 34 条、159 条向量里几何层见到的 59 条命令、十一整帧用例的 17 条命令，合计 1020 + 338 + 5224 条记录逐字段比、每个描述符过 D3 规矩 1、D7 的读源/读目的有无、零面积命令的完成脉冲与命令索引；握手监视器查多发的描述符与非空命令的 `cmd_done`，B4 行缓冲替身查每一行都开在空着的那一半上。另加 `+STALL`（三个方向各压反压）与 `+RFSTALL`（行缓冲替身的排空拉长到 23 拍），判据是序列与恒 ready 时相同。五个注错，探针 14/14，全套 27/27。不需要 `EFINITY_HOME`。见 `../../Engineering/TechReports/engine/03-几何层.md` |

## 规则

每个 `run.sh` 除了正常用例，还必须带注错用例：把一处改错，对应的判据必须报 FAIL。全部用例都符合预期才输出 `SIM OK`，否则退出码非零。只跑正常用例无法说明判据真的在检查东西。

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
| `engine/mover/inject_nogate_rd` | 去掉读方向的在途门控（`INJECT=1`），读核一 ready 就放下一个 | FAIL，读流上下文错位（拍子带的目标缓冲号与命令索引属于另一笔），总线侧也有违例周期 |
| `engine/mover/inject_mask` | 带内掩码旁路（`INJECT=2`），`wstrb` 与 X 字节直通 | FAIL，被标记「不写」的像素被写进了目的区 |
| `engine/mover/inject_nodrain` | 排空旁路（`INJECT=4`），排空期不帮忙收读流、也不补哑拍 | FAIL，两个方向都停在忙态，排空完成等不到 |
| `engine/mover/inject_nogate_wr` | 去掉写方向的在途门控（`INJECT=8`） | FAIL，上一笔写还没结束就接受了下一个写描述符 |
| `engine/mover/inject_drain_level` | 排空按电平重进（`INJECT=16`），且只在排空态里挡提交 | FAIL，请求举着时 `drain_done` 打了两次，退出排空那一拍接受了新的读描述符 |
| `engine/geom/inject_nosrc` | `engine_geom` 的 `INJECT=1`：求交跳过第五项（源表面） | FAIL，逐条比对见红：矩形变大，边带、读描述符、写描述符都不同 |
| `engine/geom/inject_no4k` | `INJECT=2`：跨 4KB 不拆，一个描述符盖过页边界 | FAIL，`RULE1 ... crosses 4KB`（定向用例里 8 处） |
| `engine/geom/inject_alpha` | `INJECT=4`：ALPHA 也按 COPY 走「不读目的」的避让 | FAIL，`MISMATCH sideband read-dst vs D7`，读描述符少一半 |
| `engine/geom/inject_extra` | `INJECT=8`：COPY 也发读目的描述符（边带仍说不读） | FAIL，握手监视器的 `PROTO handshake count`（消费循环看不见多发的描述符） |
| `engine/geom/inject_norf` | `INJECT=16`：行开始之前不看 `row_free`，配 `+RFSTALL` | FAIL，行缓冲替身的 `BUFERR ... started on busy half` |
| `engine/geom` 的探针注错 | `probe_geom.v` 的 `INJECT`（跨 4KB 不拆、漏第五项、`probe_geom_d7` 的 ALPHA 误避让） | FAIL，各自红在探针自己的判据上（14/14 → 见红） |

后两个 `apb_probe` 注错对应的是顶层曾经真实存在的 bug：APB 信号在顶层没有声明，被当成 1 位线，32 位地址和数据都被截断。

`axi_mux_guard` 的其余用例不是注错：`grant_sequence` 复现仲裁的固定授权序列；
`dual_direction` 验两个方向的保护不会互相提前解除，并检查同一拍两个方向都触发时 `GUARD_HITS` 加 2、
两个方向都还没握手时 `S2_MAX_WAIT_AR/AW` 已经在涨（guard VERSION 2 每拍更新；版本 1 在这一项读到 0）；
`normal` 与 `inflight1` 分别验读方向上保护开启与只靠措施一的口径；`inject_midgrant` 验 guard 不伪造抢占；
写方向 `wr_aggr_guard`/`wr_aggr_noguard`/`wr_contract` 在原从机上、`wr_q4_*` 三例在写队列 4 的从机上各跑一组，
全部期望 PASS，并由 `run.sh` 比对保护开、关两例的统计必须逐项相同（写方向上 guard 没有可观测的作用，
写方向的等待靠措施一）。全部全链路用例都要求从机的 `wlast_err` 为 0。
口径、实测值与写方向结论的订正见
[platform 第 06 章](../../Engineering/TechReports/platform/06-Axi_Mux三端口仲裁与SoC防饿死保护.md)。

## 范围

DDR3 控制器与官方的 `axi_interconnect_beta` 是加密 RTL，SoC 网表里也有两个 CPU 运算模块（`EfxCPUSp1`、`EfxCPUSp2`）加密，开源工具读不了，所以整机仿不了。能仿的只有明文模块；第三方的 `Axi_Mux` 是明文，但例化了厂商原语 `EFX_SRL8`，要带 Efinity 的原语仿真模型。
扫描输出的异步 FIFO 用的是 `rtl/vendor/ddr3_controller/common/efx_fifo_v2.3/efx_fifo_wrapper.v`：厂商文件，但是明文、不例化任何原语，iverilog 直接能编，所以 `display/` 不需要 `EFINITY_HOME`。
引擎开发阶段要自己写 AXI 从机模型，并且必须带随机反压和可配的响应延迟，测试台里的从机比真从机强等于没验。

`axi_ram_model.v` 是 `axi_probe`、`axi_mux_guard`、`display` 与 `engine/mover` 共用的从机，
它的 `ADDR_STALL_MAX`、`RD_QUEUE`、`WR_QUEUE`（读、写地址队列深度）与 `hold_ar`/`hold_aw`
都带默认值，默认值保持原来只有随机反压的行为不变；
改它之后用到它的仿真都要复跑（2026-09-19 加 `WR_QUEUE` 与 `wlast_err` 之后，`axi_probe` 五个用例的日志与改前逐字节相同；
2026-09-30 引擎 B2 补四样之后，`axi_probe`、`axi_mux_guard`、`display` 三个套件的完整输出与改前逐字节相同；
同日复核修了定时模式的三处，只动 `TIMED=1` 的分支，三个套件重跑仍逐字节相同）。

2026-09-30（引擎 P1 B2，D13）补的四样，全部是新增参数、默认值保持原行为：

| 参数 | 默认 | 作用 |
| --- | --- | --- |
| `MEM_WORDS` | 4096 | 存储字数，地址取模改用它；整帧用例的设备内存 4 MiB 时取 262144 |
| `STRB_EN` | 0 | 0 按整字写（原行为）；1 按 `wstrb` 逐字节写。`wstrb` 是新增端口，默认不读它，所以既有测试台不接这个端口也保持原行为 |
| `ERR_LO`/`ERR_HI` | `0xFFFF_FFFF`/`0` | 落在这个字节区间的突发回 `SLVERR`，默认值让判断恒为假 |
| `TIMED` | 0 | 0 随机反压（原行为）；1 定时模式，用 `RD_LAT=23 RD_BUB=12 WR_BUB=6 B_LAT=1`（E4 校准值）：第一个 R 握手在 AR 握手之后第 `RD_LAT+2` 拍，W 通道每收满 `WR_BUB` 拍空一拍，最后一拍之后 `B_LAT` 拍出写响应。与前期研究校准用的从机逐拍相同，回归是 `engine/mover/tb_slave_timed.v` |

从机发现 WLAST 与突发长度对不上时打印 `MODEL: wlast mismatch` 并把 `wlast_err` 加一，
用到它的测试台必须把 `wlast_err` 非零判为失败：只打印不计错，会让一个把写通路弄死锁的主机替身看起来像「CPU 被饿死」。

产物在各子目录的 `build/` 下，不进版本库。
