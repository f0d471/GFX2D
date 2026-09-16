# rtl：硬件设计源码

顶层是 `top/gfx2d_top.v`，模块名 `gfx2d_top`。Efinity 工程在 `fpga/gfx2d_ti60/`，源文件列表写在 `gfx2d_ti60.xml` 里，本目录下的文件不是全部都在列表中，下表逐个注明。

`fpga/gfx2d_ti60/` 与 `rtl/` 的相对位置不能改：工程用 `../../rtl/...` 引用源码。

## 目录

| 目录 | 内容 | 来源 | 是否可改 |
| --- | --- | --- | --- |
| `top/` | 顶层 | 本项目 | 可改 |
| `bus/` | 挂在 SoC 总线上的从设备 | 本项目 | 可改 |
| `hdmi/` | HDMI 发送通路 | 封装为本项目编写，编码器取自官方例程 | 封装可改，官方文件不改 |
| `vendor/` | 厂商与第三方代码 | 见下 | 原则上不改，改了要在本文件登记 |

## top/

| 文件 | 说明 |
| --- | --- |
| `gfx2d_top.v` | 顶层。例化 SoC（`u_soc`）、DDR3 控制器（`u_ddr3`）、APB 探针（`u_apb_probe`）、HDMI 发送（`u_hdmi_tx`） |

端口名与 `fpga/gfx2d_ti60/gfx2d_ti60.peri.xml` 中的信号名一一对应，改一边必须同步另一边，否则 Interface Designer 会报 Missing Interface Pins。

文件开头用 `` `default_nettype none ``，结尾恢复 `wire`。工程的综合选项是同一个编译单元（`hdl-compile-unit=1`），不恢复会影响后面编译的厂商文件。

SoC 与 DDR3 控制器之间有两处有意的位宽截断，在文件里也写了：地址 32 位接 28 位（DDR3 只有 256MB），ID 8 位接 4 位（回来的响应高位补 0）。

## bus/

| 文件 | 说明 |
| --- | --- |
| `apb_probe.v` | APB 探针寄存器，挂 APB Slave 0（CPU 地址 `0xF810_0000`，64KB 窗口）。`0x000` ID（只读，"GFX2"）、`0x004` VERSION（只读）、`0x008` SCRATCH（读写）；其余地址读回 0 并置 PSLVERROR |

加速器的控制寄存器将来接在同一条 APB 上，这个模块是占位，也是 APB 通路的自检手段：上板用 OpenOCD 直接读写这几个地址即可，不需要写程序。仿真见 `sim/apb_probe/`。

## hdmi/

| 文件 | 来源 | 说明 |
| --- | --- | --- |
| `hdmi_tx.v` | 本项目 | 封装：复位同步、彩条、TMDS 编码、P/N 取反。参数化行场时序，默认 640x480@60 |
| `color_bar_rgb.v` | 官方 `03_hdmi_tx_demo` | 行场时序与八色竖条发生器 |
| `dvi_encoder.v` | 官方 `03_hdmi_tx_demo` | 三通道 TMDS 编码 |
| `encode.v` | 官方 `03_hdmi_tx_demo` | 单通道 8b/10b 编码 |

三个官方文件与资料包中的原件 MD5 一致。仿真见 `sim/hdmi/`。

将来扫描输出改为从 DDR3 读帧缓冲时，替换的是 `color_bar_rgb`，编码器与 `hdmi_tx` 的对外接口不动。

## vendor/

| 目录 | 内容 | 状态 |
| --- | --- | --- |
| `ddr3_controller/` | 易灵思 DDR3 软核控制器。顶层 `ddr3_top.v`，参数 `ddr3_parameter.vh`，其余为 PHY、bank 管理、AXI 从口 | 在工程源文件列表中。多数文件是加密 RTL，开源仿真工具读不了 |
| `axi_mux/` | 第三方明文 AXI 多路器（2~8 主机进、1 出，128 位），来自官方工程 | **不在**源文件列表中。接引擎共享 DDR3 时启用 |

两处要注意的：

- `ddr3_controller/common/simple_dual_port_ram.v` 用的是官方工程 `rtl/common/` 下那一份（1565 字节），也就是当前上板比特流实际编进去的版本。厂商目录里原本自带另一份同名文件（3413 字节，未被工程引用），已被这一份覆盖，原文件在 git 历史里。保留两份会出现同名模块，容易编错。
- `axi_mux/Axi_Mux.v` 开头有 `` `define X2 ``（选择 2 个主机端口），工程是单编译单元，启用后这个宏会对后面编译的文件可见，接入时要确认没有冲突。

## 关键参数

DDR3 的位宽与时序都在 `vendor/ddr3_controller/ddr3_parameter.vh`：行地址 14 位、bank 3 位、列 10 位、数据 16 位，AXI 侧地址 28 位、数据 128 位、ID 4 位，`BURST_MODE="8"`，`ASYN_AXI_CLK=0`（AXI 与 core_clk 同域）。
