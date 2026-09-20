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
| `display/` | `rtl/display/` 的扫描输出：`display_fifo`（厂商异步 FIFO 的封装）与 `display_top` | 两个测试台。`tb_fifo_semantics`：正常 + `inject_no_reset`；`tb_display`：`image`（含像素时钟 ±0.5% 两组）、`swap`、`inflight`、`level`、`starve`、`snapshot`、`image_full`（整帧 640×480，用 `FULL=1` 打开）与 7 个注错。见 `../../Engineering/Todo/hw/扫描输出.md` 2.3 节。只有明文 RTL，不需要 `EFINITY_HOME` |

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

后两个 `apb_probe` 注错对应的是顶层曾经真实存在的 bug：APB 信号在顶层没有声明，被当成 1 位线，32 位地址和数据都被截断。

`axi_mux_guard` 的其余用例不是注错：`grant_sequence` 复现仲裁的固定授权序列；
`dual_direction` 验两个方向的保护不会互相提前解除，并检查同一拍两个方向都触发时 `GUARD_HITS` 加 2、
两个方向都还没握手时 `S2_MAX_WAIT_AR/AW` 已经在涨（guard VERSION 2 每拍更新；版本 1 在这一项读到 0）；
`normal` 与 `inflight1` 分别验读方向上保护开启与只靠措施一的口径；`inject_midgrant` 验 guard 不伪造抢占；
写方向 `wr_aggr_guard`/`wr_aggr_noguard`/`wr_contract` 在原从机上、`wr_q4_*` 三例在写队列 4 的从机上各跑一组，
全部期望 PASS，并由 `run.sh` 比对保护开、关两例的统计必须逐项相同（写方向上 guard 没有可观测的作用，
写方向的等待靠措施一）。全部全链路用例都要求从机的 `wlast_err` 为 0。
口径、实测值与写方向结论的订正见
[`../../Engineering/Todo/hw/E5-Axi_Mux优先级与扫描FIFO水位.md`](../../Engineering/Todo/hw/E5-Axi_Mux优先级与扫描FIFO水位.md) 2.2 节
与 platform 第 06 章。

## 范围

DDR3 控制器与官方的 `axi_interconnect_beta` 是加密 RTL，SoC 网表里也有两个 CPU 运算模块（`EfxCPUSp1`、`EfxCPUSp2`）加密，开源工具读不了，所以整机仿不了。能仿的只有明文模块；第三方的 `Axi_Mux` 是明文，但例化了厂商原语 `EFX_SRL8`，要带 Efinity 的原语仿真模型。
扫描输出的异步 FIFO 用的是 `rtl/vendor/ddr3_controller/common/efx_fifo_v2.3/efx_fifo_wrapper.v`：厂商文件，但是明文、不例化任何原语，iverilog 直接能编，所以 `display/` 不需要 `EFINITY_HOME`。
引擎开发阶段要自己写 AXI 从机模型，并且必须带随机反压和可配的响应延迟，测试台里的从机比真从机强等于没验。

`axi_ram_model.v` 是 `axi_probe` 与 `axi_mux_guard` 共用的从机，它的 `ADDR_STALL_MAX`、`RD_QUEUE`、`WR_QUEUE`
（读、写地址队列深度）与 `hold_ar`/`hold_aw` 都带默认值，默认值保持原来只有随机反压的行为不变；
改它之后两个仿真都要复跑（2026-09-19 加 `WR_QUEUE` 与 `wlast_err` 之后，`axi_probe` 五个用例的日志与改前逐字节相同）。
从机发现 WLAST 与突发长度对不上时打印 `MODEL: wlast mismatch` 并把 `wlast_err` 加一，
用到它的测试台必须把 `wlast_err` 非零判为失败：只打印不计错，会让一个把写通路弄死锁的主机替身看起来像「CPU 被饿死」。

产物在各子目录的 `build/` 下，不进版本库。
