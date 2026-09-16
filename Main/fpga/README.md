# fpga：Efinity 工程

| 目录 | 说明 |
| --- | --- |
| `gfx2d_ti60/` | 唯一的 Efinity 工程。器件 Ti60F225，时序模型 I3，顶层模块 `gfx2d_top` |

`gfx2d_ti60/` 与 `rtl/` 的相对位置不能改：工程用 `../../rtl/...` 引用源码，工程目录必须在仓库下两级。

## 文件

| 路径 | 内容 |
| --- | --- |
| `gfx2d_ti60/gfx2d_ti60.xml` | 工程文件：源文件列表、综合与布局布线选项、比特流选项、约束文件路径 |
| `gfx2d_ti60/gfx2d_ti60.peri.xml` | 接口设计：管脚分配、IO 电压、PLL、LVDS TX、硬 JTAG |
| `gfx2d_ti60/sdc/gfx2d_ti60.sdc` | 时序约束 |
| `gfx2d_ti60/ip/soc/` | Sapphire SoC IP 的生成结果（`settings.json` 是配置，`soc.v` 是网表） |

编译产物 `outflow/`、`work_*/` 不入版本库。

## 编译

```
call <EFINITY>\bin\setup.bat
cd fpga\gfx2d_ti60
efx_run.bat gfx2d_ti60.xml
```

map、interface、pnr、pgm 四步，约一分半，输出 `outflow/gfx2d_ti60.bit`（JTAG 加载）与 `.hex`（写 Flash）。不需要开 GUI，也不需要许可证。

编译与上板的结果见 `../../Engineering/TechReports/platform/01-从零搭起可上板的硬件基线.md` 第五节，操作命令见 `../doc/开发环境.md`。

## 三个 PLL

| 实例 | 资源 | 参考时钟 | 输出 | 用途 |
| --- | --- | --- | --- | --- |
| `pll_inst1` | PLL_TL0 | `clk_25m`（J2） | `core_clk` 100MHz | SoC、DDR3 控制器 AXI 侧、APB |
| `DDR3_PLL` | PLL_TR0 | `clk_25m` | `sdram_clk`、`rx_cal_clk`、`tx_cal_clk`、`tx_cal_clk_90edge` 各 400MHz，`clk_100m` 100MHz | DDR3 PHY |
| `HDMI_PLL` | PLL_BL0 | `clk_27m`（H2） | `hdmi_tx_slow_clk` 25.2MHz、`hdmi_tx_fast_clk` 126MHz（相移 90°） | HDMI 像素与串行化 |

Ti60 的 PLL 有两条容易踩的规则：静态相移的单位是 0.5 个 FPLL 周期（不是 45°），CLKOUT0 的频率不能超过参考时钟的 4 倍。25MHz 参考给不出 25.2MHz 加 90° 的组合，所以 HDMI 用 27MHz 晶振。推导过程见 `../../Engineering/TechReports/platform/01-从零搭起可上板的硬件基线.md` 3.4 节。

## 管脚

已分配：

| 信号 | 管脚 | 说明 |
| --- | --- | --- |
| `clk_25m` / `clk_27m` | J2 / H2 | 25MHz、27MHz 有源晶振 |
| `system_uart_0_io_rxd` / `txd` | R4 / R14 | 板上 FT4232 的 C 通道，串口号见 `../doc/开发环境.md` 第 4 节 |
| `soc_gpio[3:0]` | P13、N13、G3、N2 | LED0~3。P13、N13 是 3.3V，G3、N2 是 1.8V |
| `system_spi_0_io_*` | GPIOL_P_03、GPIOL_N_03、GPIOL_N_01、GPIOL_P_01 | 配置 Flash，只连 data0/1 |
| `tmds_tx_*` | GPIOR_PN_10/13/11/12 | HDMI TX 四对差分，板上 P/N 与 FPGA 对调 |
| `ddr_*` | 见 `peri.xml` | DDR3，1.5V SSTL |
| `jtag_inst1_*` | 硬 JTAG 用户 TAP | 无外部管脚，走 JTAG_USER1 |

未接入、将来要用的：

| 板上信号 | 管脚 | 说明 |
| --- | --- | --- |
| KEY0~3 | C4、P14、N14、A3 | 3.3V。游戏输入要用，接进 SoC 时新开 GPIO1 或把 GPIO0 加宽 |

2026-09-16 从接口设计里删掉的条目：`i_arstn`（C4，登记了但网表里没有）、`io_jtag_tck/tdi/tdo/tms`（A13、B12、C12、A14，软 JTAG 预留，不使用）、`ddr_addr[15:14]`（控制器行地址只有 14 位）。删除前它们都出现在布局报告的 Missing Interface Pins 里。

## 改动约定

- 改顶层端口要同步改 `peri.xml`，反过来也一样。对不上时 Interface Designer 会在 Missing Interface Pins 里列出来，但不会报错，容易漏。
- 改 PLL 参数要同步改 `sdc/gfx2d_ti60.sdc` 里的时钟周期与波形。
- `settings.json` 里 `--base_path` 和 `HexFile_Path` 必须是绝对路径。仓库里存的是占位符，克隆之后用 `tools/localize_paths.py --apply` 展开，提交前用 `--restore` 还原，见 `../doc/开发环境.md` 第 3 节。
- 调试核：`<efx:debugger>` 的 `auto_instantiation` 是 `off`。打开的话，命令行编译会失败，因为自动插入这一步要求 `work_dbg/debug_top.v` 事先存在，而这个文件只有 GUI 的 Debug Wizard 会生成。要用逻辑分析仪时，在 GUI 里生成一次再编译，原因见 `../../Engineering/TechReports/platform/01-从零搭起可上板的硬件基线.md` 4.2 节。

## SoC IP

`ip/soc/settings.json` 的关键项：efx_soc 3.3.0、单核 100MHz、指令与数据缓存各 4KB、无 FPU、DDR 接口 128 位、APB Slave 0 开 64KB、AXI 从口窗口 16MB、I2C0 与用户中断 A 已生成。程序区 `LDSize=4092`，片上 RAM 启用自定义 hex（512KB bootloader）。

Efinity 2026.1 自带 efx_soc 3.4.0，用它在 GUI 里打开本工程会提示升级 IP。升级会重新生成 `ip/soc/` 和软件包，`sdk/` 里登记过的修改要重新应用，见 `sdk/README.md`。升级前先提交或备份。
