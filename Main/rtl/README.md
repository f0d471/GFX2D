# rtl：硬件设计源码

顶层是 `top/gfx2d_top.v`，模块名 `gfx2d_top`。Efinity 工程在 `fpga/gfx2d_ti60/`，源文件列表写在 `gfx2d_ti60.xml` 里，本目录下的文件不是全部都在列表中，下表逐个注明。

`fpga/gfx2d_ti60/` 与 `rtl/` 的相对位置不能改：工程按两个目录的相对路径引用源码。

## 目录

| 目录 | 内容 | 来源 | 是否可改 |
| --- | --- | --- | --- |
| `top/` | 顶层 | 本项目 | 可改 |
| `bus/` | 挂在 SoC 总线上的从设备 | 本项目 | 可改 |
| `hdmi/` | HDMI 发送通路 | 封装为本项目编写，编码器取自官方例程 | 封装可改，官方文件不改 |
| `display/` | 扫描输出（display） | 本项目 | 可改 |
| `engine/` | 渲染引擎 | 本项目 | 可改 |
| `vendor/` | 厂商与第三方代码 | 见下 | 原则上不改，改了要在本文件登记 |

## display/

| 文件 | 说明 |
| --- | --- |
| `display_top.v` | 扫描输出的对外入口：APB 从口、AXI 读主机口（Axi_Mux 的 S0）、中断，以及给 `hdmi_tx` 的像素与同步。里面三块 |
| `display_fetch.v` | 取数侧（`core_clk`）：AXI 读主机、按水位取数、场起始清空的六步、APB 寄存器与统计、帧快照接收 |
| `display_fifo.v` | 跨时钟的异步 FIFO：仓内厂商明文源码 `efx_fifo_wrapper` 的薄封装（`SYNC_CLK=0`、512 深 × 128 位、`OUTPUT_REG=0`），定值与三条纪律写在文件头 |
| `display_out.v` | 像素侧（像素时钟）：唯一的视频时序发生器（例化官方 `color_bar_rgb`）、拍拆像素、D=1 对齐、`SRC` 二选一、水位与欠载观测、帧快照 |

`display_fetch`/`display_out` 各有若干处注错（变异测试），各包在 `` `ifdef INJ_xxx `` 内：不定义时
那些行进不了编译输入，RTL 的干净形态就是正式配置。开关名见两个文件的文件头，
用它的地方只有 `sim/display/run.sh`；机制与提交前检查见
[`../doc/开发环境.md`](../doc/开发环境.md) 0.20 节。
设计依据与寄存器表见 `display/display-manual.md`。

## engine/

引擎按混合、搬运、几何、计算、控制五层加顶层集成分块，层间接口契约见
`engine/engine-manual.md` 第 4 节；片上纹理（TEX）装载窗口与条带缓冲在此基础启用，
机制见 `engine-manual.md` 与各文件头注释。

| 文件 | 说明 |
| --- | --- |
| `engine_bin.v`、`engine_bounds.v`、`engine_packet_check.v` | 硬件分桶前端：COUNT/快照、顺序PREFIX与保序SCATTER；18位求交、共享字段校验、32×96桶RAM；引用保持完整逻辑身份，READY首拍最后发布 |
| `engine_rlst.v`、`engine_rast.v` | 三维单元：64 项索引 FIFO、128B 双参数槽、跨页取数、4 DSP 位置求值、有符号增量扫描、20 RAM 深度条带、固定延迟 TEX 与色/z 原子提交。以 `rlst`、`rast-ports`、`rast-rtl` 检查；尚未进入生产预设或 SoC 调度，端口契约见 `engine-manual.md` 第 8 节 |
| `engine_pb.v`、`engine_pb_dma.v` | 硬件分桶前端：PB配置/预检、逐桶LOAD/引用/metadata/坐标/旧执行器/FLUSH；复用DMA owner，PB原样写全部位，错误/复位排空 |
| `engine_inst.v` | 实例命令前端：v1-I 直通实例展开，单个 256×32 坐标缓存、2 RAM，显式 DMA owner、跨 4 KiB 取数和首失败实例定位；已进入整机网表 |
| `engine_tex.v` | 片上纹理：16 KiB 四 bank scratchpad，32 位 APB 装载/读回，128 位同步读流，读延迟可选 1/2 拍；状态保持到 ready。cmd/geom 传 src_tex/rd_tex，mover 允许 DDR 请求提前而保持读流单所有者顺序；已进 SoC 网表 |
| `engine_band.v` | 条带数据面：640×16×32 位，四路掩码、2048+512 分段、36 RAM；三相、紧凑映射、本地状态、同址读优先和取消。mover 目的路由与 geom 条带求交已实现，帧级控制已接、CAPS.BAND=1，描述符入口寄存一拍；17 项验证和独立综合通过，2026-10-08 已验收 |
| `engine_blend.v` | 单通道混合核。源/目的/`a`/`c` 各 8 位与方程号 3 位进去，8 位出来，固定延迟 `L = 3`。六个方程都写成 `x = base × 255 + m1 × m2`，每通道一个推断出来的 `EFX_DSP48`；`base × 255` 用移位相减。`engine_pix.v` 例化 12 个实例，已随引擎接入整机网表 |
| `engine_mover.v` | 搬运层（AXI 事务层）。引擎唯一的 AXI 主机，接 `Axi_Mux` 的 S1。里面是 `vendor/verilog-axi/` 的两个核（源码一字不改）、读侧仲裁（命令取数优先）、每方向一笔在途的门控、W 通道带内掩码、复位排空（一次请求只完成一次，请求举着期间不收新提交）与超时。读方向的「完成」是**状态脉冲已捕获且读流 `tlast` 已握手**（读核的状态可以早于末拍握手 1~55 拍，只看状态会让上下文错位）。整块挂搬运层复位（顶层接 `~ddr_rstn`）。`engine_top.v` 已例化并接入 SoC 网表 |
| `engine_geom.v` | 几何层。接一条过了十步检查的 FILL 或 BLIT，五者求交、逐行迭代（每行 4 拍）、按 16 字节对齐的覆盖区间发读源、读目的、写目的的描述符（不跨 4KB、不超过 192 拍），每行先发一条边带；`rd_buf = {源/目的, 行号最低位}`，每一行开始之前查 `row_free[行号最低位]`；零面积命令回一拍 `cmd_done`。`engine_top.v` 已例化并接入 SoC 网表 |
| `engine_pix.v` | 计算阵列与片上缓存。源 512×128、目的 512×96（X 字节不存）两块行缓冲，各放两行乒乓；两拍窗口的像素旋转、首末拍掩码与色键（比原始存储字），在 X 字节最高位打「不写」标记；12 个 `engine_blend` 的四像素通路，FILL 的 a 一律取 `w4[31:24]`；七拍流水挂一个时钟使能 `en = ws_tready \|\| !ws_tvalid`，写核停时整条停。读流恒 ready，写流没有 `tlast`（由搬运层按拍数打），`row_free[1:0]` 在该半写流末拍被写核收走之后拉高。`engine_top.v` 已例化并接入 SoC 网表 |
| `engine_top.v`、`engine_regs.v`、`engine_cmd.v` | 控制层：APB 寄存器、命令表批量取数、十步检查、错误三元组与性能计数；`engine_top` 例化混合/搬运/几何/计算四层，各文件已列进工程源文件列表。159 向量端到端与定向用例、单独综合和板卡基线均已验收，已接入 SoC 顶层。条带表头/三相循环、局部 y 转换、反馈拒绝及排空已并入，见 `engine-manual.md`。寄存器偏移由 `tools/check/regs_gen.py` 生成，不手改 `.vh` |
| `engine_feature.vh`、`engine_config_guard.vh`、`engine_config_error_modules.vh`、`engine_regs_offsets.vh` | 生成物，由 `tools/check/feature_build.py` 写出，不手改。四份都落在本目录，所以 RTL 用同目录 `` `include "engine_feature.vh" `` 即可，不需要额外的 include 路径；`--sync` 重生成、`--check` 校验 |

**每个引擎 RTL 文件都必须 `` `include "engine_feature.vh" ``**（放在文件头、`` `default_nettype `` 之后）。
可选机制由它定义的宏进出编译输入：`` `ifdef GFX_HAS_xxx `` 命中即该机制存在，不定义即整块离开编译输入，
不做「输入接常量、寄存器照留」的裁剪。九个自写文件当前都带这一行，新增文件要一并加上，
否则构型构建时那些 `` `ifdef `` 会被当成未定义而静默走关闭分支。
构型定义、依赖边、CAPS 编码与七个代表档见 [`../tools/check/feature_config.py`](../tools/check/feature_config.py)；
生成、同步与校验用 [`../tools/check/feature_build.py`](../tools/check/feature_build.py)，
按构型的判据入口见 [`../tools/check/README.md`](../tools/check/README.md)。

混合、搬运、几何、计算、控制与片上纹理的六个文件各有若干处注错（变异测试），各包在 `` `ifdef INJ_xxx `` 内：
不定义时那些行进不了编译输入，RTL 的干净形态就是正式配置，不依赖综合器把常量分支优化掉。
开关名见各自文件头，用它的地方只有 `sim/engine/` 下各自目录的 `run.sh`（计算阵列是 `run_formal.sh`）。
机制、命名与提交前检查见 [`../doc/开发环境.md`](../doc/开发环境.md) 0.20 节。

各层的层间协议、编码表与注错清单见 `engine/engine-manual.md`。

## top/

| 文件 | 说明 |
| --- | --- |
| `gfx2d_top.v` | 顶层。例化 SoC（`u_soc`）、DDR3 控制器（`u_ddr3`）、渲染引擎（`u_engine`）、真实扫描输出（`u_display`）、AXI 多路器（`u_axi_mux`）、仲裁配置与防饿死保护（`u_mux_guard`）、HDMI 发送（`u_hdmi_tx`） |

端口名与 `fpga/gfx2d_ti60/gfx2d_ti60.peri.xml` 中的信号名一一对应，改一边必须同步另一边，否则 Interface Designer 会报 Missing Interface Pins。

文件开头用 `` `default_nettype none ``，结尾恢复 `wire`。工程的综合选项是同一个编译单元（`hdl-compile-unit=1`），不恢复会影响后面编译的厂商文件。

SoC 经 `u_axi_mux` 接 DDR3 控制器，中间有两处有意的位宽截断，在文件里也写了：地址 32 位接 28 位（DDR3 只有 256MB），ID 8 位接 4 位（回来的响应高位补 0）。两处都已查证安全：`0x0000_1000`~`0x0FFF_FFFF` 内 CPU 地址即控制器地址，以上回绕；SoC 发出的 ID 恒为 0。前提是 SoC 的外部主机口 `io_ddrMasters_0` 不用，它的 ID 最高位是 1；2026-09-19 起这个口已在 SoC IP 里关掉。

SoC 的其余连接（2026-09-19 SoC IP 定型之后）：GPIO1 读口 [3:0] 接顶层 `key_n[3:0]`（KEY0~3，低有效），[7:4] 接 0，写口悬空，[4] 预留给摄像头复位；
用户中断 A/B/C（中断号 16/17/22）分给扫描输出、引擎、摄像头；A/B 已接各自的 IRQ，C 因摄像头尚未接入而接 0；
I2C0（`0xF801_7000`）预留给摄像头，读口接 1、写口悬空；`axiA` 从口已关掉。分配表见 `../fpga/README.md`「SoC IP」。

## bus/

| 文件 | 说明 |
| --- | --- |
| `apb_probe.v` | APB 探针寄存器（占位并验证 APB 通路）。**已不在顶层例化**：它占的 APB 第 0 页已归引擎，顶层未映射页改为常量应答（`PREADY=1`、`PSLVERR=1`、`PRDATA=0`）。源文件与 `sim/apb_probe/` 保留。`0x000` ID（只读，"GFX2"）、`0x004` VERSION（只读）、`0x008` SCRATCH（读写）；其余地址读回 0 并置 PSLVERROR |
| `axi_probe_master.v` | AXI 探针主机，DDR3 上的第二个 AXI 主机。**已不在顶层例化**：它占的 Axi_Mux S1 已归引擎。寄存器表在文件头（APB `0x1000~0x1FFF`，随之不可访问）。源文件与 `sim/axi_probe/` 保留 |
| `axi_mux_guard.v` | `Axi_Mux` 的仲裁方式、三通道用户掩码与 SoC（S2）地址等待的防饿死保护，永久模块。挂 APB Slave 0 的 `0xF000~0xFFFF`（CPU 地址 `0xF810_F000`），寄存器表在文件头，当前 `VERSION` 为 2（两个 `S2_MAX_WAIT` 每拍更新，含尚未握手的那一笔）。写方向上 guard 没有可观测的作用，写方向的等待靠主机侧「每方向一笔在途」的契约 |
| `scanout_stub.v` | 扫描替身：按行节拍发 160 拍读，记录等待与间隙。真实 `display` 已接管 APB `0xE000~0xEFFF` 与 Axi_Mux S0；本文件仍列在 Efinity 工程源列表中，但顶层不例化，并保留给 `sim/axi_mux_guard/` 使用 |

引擎的控制寄存器已接在同一条 APB 第 0 页（`rtl/engine/`；未映射页由顶层常量应答）；上板用 OpenOCD 直接读写即可，不需要写程序，回归表见 `doc/开发环境.md`。仿真见 `sim/apb_probe/`。

## hdmi/

| 文件 | 来源 | 说明 |
| --- | --- | --- |
| `hdmi_tx.v` | 本项目 | 纯编码封装：复位同步（导出的 `vid_rst_n` 给像素侧共用）、TMDS 编码、P/N 取反。像素与行场同步由 `display_out` 给（`vid_rgb`/`vid_hs`/`vid_vs`/`vid_de`）。行场参数表保留在它的参数表里，但已不再使用，真正生效的一份在 `display_out` |
| `color_bar_rgb.v` | 官方 `03_hdmi_tx_demo` | 行场时序与八色竖条发生器。2026-09-20 起由 `display_out` 例化，是全设计唯一的时序发生器 |
| `dvi_encoder.v` | 官方 `03_hdmi_tx_demo` | 三通道 TMDS 编码 |
| `encode.v` | 官方 `03_hdmi_tx_demo` | 单通道 8b/10b 编码 |

三个官方文件与资料包中的原件 MD5 一致。仿真见 `sim/hdmi/`。

彩条搬到了 `rtl/display/display_out.v`：
这样「场」只有一个真源，彩条与帧缓冲两条通路共用同一个编码器，唯一的差别就是像素来源。
`sim/hdmi/tb_hdmi_tx.v` 因此自己例化一个彩条来驱动 `hdmi_tx` 的像素输入，三个用例的判据一条没改。

## vendor/

| 目录 | 内容 | 状态 |
| --- | --- | --- |
| `ddr3_controller/` | 易灵思 DDR3 软核控制器。顶层 `ddr3_top.v`，参数 `ddr3_parameter.vh`，其余为 PHY、bank 管理、AXI 从口 | 在工程源文件列表中。多数文件是加密 RTL，开源仿真工具读不了 |
| `axi_mux/` | 第三方明文 AXI 多路器（2~8 主机进、1 出，128 位），来自官方工程，官方设计里没有例化过它 | 在工程源文件列表中，例化为 `u_axi_mux`；S0 接真实 `display`（此前是扫描替身），S1 接 `engine_top`（此前是探针主机），S2 接 SoC。除第 43 行的宏以外未改动。没有 BRESP 端口，顶层把控制器的 bresp 并接给各主机；`EFX_SRL8` 的输出取反、地址反向，仿真必须用厂商模型 |
| `verilog-axi/` | 第三方明文 AXI DMA 核：`axi_dma_rd.v`（读）、`axi_dma_wr.v`（写），来自 [alexforencich/verilog-axi](https://github.com/alexforencich/verilog-axi) 归档前最后一个提交 `516bd5dadc3365b7f9e225d2af8fe0b8d804fe53`（2025-02-27），MIT 许可。三个文件都与上游逐字节相同（git blob 与上游提交里的相同） | 两个 RTL 已列入工程源文件列表并由 `engine_mover.v` 例化。`LICENSE` 是上游的 `COPYING`，只是换了个更好找的名字，内容一字不改。`engine_mover.v` 用参数把它们配到本工程的位宽与突发上限，源码本身一字不改 |

两处要注意的：

- `ddr3_controller/common/simple_dual_port_ram.v` 用的是官方工程 `rtl/common/` 下那一份（1565 字节），也就是当前上板比特流实际编进去的版本。厂商目录里原本自带另一份同名文件（3413 字节，未被工程引用），已被这一份覆盖，原文件在 git 历史里。保留两份会出现同名模块，容易编错。
- **`axi_mux/Axi_Mux.v` 第 43 行改过一处**：2026-09-19 把 `` `define X2 `` 改成 `` `define X3 ``，启用现成的第三个主机端口（端口列表 164~199 行、声明 552~591 行、输入选择 911~936 行、输出解复用 1373~1386 行都是厂商写好的 `ifdef X3` 块），仲裁状态机一行未动。`X3` 会连带定义 `X2` 并把 `Axi_Chnanel_Num` 定为 3，因而 `Cfg_Cnl_Mask` 由 2 位变 3 位、`O_*_Cnl_Num` 由 1 位变 2 位（顶层这五个输出仍是空连接）。工程是单编译单元，这个宏对后面编译的文件可见；2026-09-16 与 09-19 各查过一次，本工程其余明文文件没有用到 `X2`~`X8` 与 `Axi_Chnanel_Num`。对照原件时只需还原这一行。

## 关键参数

DDR3 的位宽与时序都在 `vendor/ddr3_controller/ddr3_parameter.vh`：行地址 14 位、bank 3 位、列 10 位、数据 16 位，AXI 侧地址 28 位、数据 128 位、ID 4 位，`BURST_MODE="8"`，`ASYN_AXI_CLK=0`（AXI 与 core_clk 同域）。
