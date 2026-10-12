# sim/engine/pix

**计算阵列与片上缓存** 的仿真目录：入库 RTL `rtl/engine/engine_pix.v` 的正式判据，
加上前期研究的探针（研究做完了，保留为回归）。资源与板上数据结论见 `syn/README.md`
与各测试台判据。

| 文件 | 是什么 |
| --- | --- |
| `run.sh` | 总入口：九项研究探针，再跑 `run_formal.sh`；两部分都符合预期才输出 `SIM OK`，现为 25/25 |
| `run_formal.sh` | 入库 RTL 的十六项判据：单元七项、乒乓五项、真实通路两项、跨命令写后读两项 |
| `tb_engine_pix.v` | 一行一行的单元测试台：sweep 400 行（`npx` 1..25 × 源/目的相位 4×4）、FILL 六方程、拆段四行、长行八行，共 418 行、10216 个槽，逐槽对照 `sw/lib/gfx/exec_sw.c` 的像素公式；FILL 的参考取常量字高字节（与审定的 RTL 一致）；近似色键字（只有高字节不同）必须照写；写流下游恒 ready 或每 7 拍停 1 拍 |
| `tb_engine_pix_pipe.v` | 乒乓与流水测试台：按边带契约允许的最快节奏发行（行与行之间不等，只等 `row_free`），命令 1~5 行、半号照几何层；期望值排队，检查端按写流握手逐拍出队比对；写流下游恒 ready、周期停顿、成串停顿三种；另查 `row_free` 占用时 `sb_ready` 为 0 |
| `tb_engine_path.v` | 引擎全链真实通路：几何层、计算阵列、搬运层、两个 DMA 核与随机反压的 AXI RAM，512 KiB 存储逐 128 位对软件执行器导出的期望内存。选项：`-DNOWAIT` 几何层一空闲就提交下一条（不等写回）；`-DCASE="build/xxx"` 换用例前缀；`-DTIMED` 从机换板上实测校准的定时模式并打印总周期数 |
| `run_frames.sh` | 导出十一个整帧用例并逐帧对 `golden.txt` 验 CRC，输出在不入库的 `build/frames/`，供整帧仿真使用 |
| `probe_pix.v` | 研究探针，**不是入库实现**：`probe_pix_linebuf`、`probe_pix_rot`、`probe_pix_mask`、`probe_pix_path` 四个模块 |
| `tb_probe_pix.v` | 研究探针的测试台：同一套 418 行用例，`-DREALWR` 接真写核 `axi_dma_wr` 与定时从机并统计停顿拍数 |
| `syn/` | 两个单独综合工程：研究探针 `probe_pix_syn.xml` 与入库实现 `engine_pix_syn.xml`，见 `syn/README.md` |

期望值都不取自被测：单元与乒乓测试台的参考模型照抄 `exec_sw.c`，真实通路的期望内存由
`tools/check/pix_cases_dump.c` 链接 `sw/lib/gfx` 经软件执行器导出（`run_formal.sh` 现编现导）。

## 跑法

```bash
wsl -e bash -c 'cd Main/sim/engine/pix && bash run.sh'
```

不需要 `EFINITY_HOME`（不经过 `Axi_Mux`）。期望最后一行 `SIM OK: 25/25 checks as expected`。
只重跑入库 RTL 用 `bash run_formal.sh`，期望 `SIM OK: 16/16 RTL checks as expected`。
整帧导出与 CRC 自检用 `bash run_frames.sh`，期望 `FRAME EXPORT OK: 11/11 CRCs`。

量两种提交方式的周期数（跨命令的写后读）：

```bash
cd "$(git rev-parse --show-toplevel)/Main/sim/engine/pix" && bash run_formal.sh >/dev/null
build/pix_cases_dump --sprites build/sprites
M="$(git rev-parse --show-toplevel)/Main"; RTL="$M/rtl/engine"
iverilog -g2012 -s tb_engine_path -DTIMED '-DCASE="build/sprites"' -o build/perf.vvp \
tb_engine_path.v "$RTL/engine_geom.v" "$RTL/engine_pix.v" "$RTL/engine_blend.v" \
"$RTL/engine_mover.v" "$M/rtl/vendor/verilog-axi/axi_dma_rd.v" \
"$M/rtl/vendor/verilog-axi/axi_dma_wr.v" "$M/sim/axi_probe/axi_ram_model.v" \
&& vvp -n build/perf.vvp | grep PERF
iverilog -g2012 -s tb_engine_path -DTIMED -DNOWAIT '-DCASE="build/sprites"' -o build/perf_nw.vvp \
tb_engine_path.v "$RTL/engine_geom.v" "$RTL/engine_pix.v" "$RTL/engine_blend.v" \
"$RTL/engine_mover.v" "$M/rtl/vendor/verilog-axi/axi_dma_rd.v" \
"$M/rtl/vendor/verilog-axi/axi_dma_wr.v" "$M/sim/axi_probe/axi_ram_model.v" \
&& vvp -n build/perf_nw.vvp | grep PERF
```

期望 `PERF cycles=17668` 与 `PERF cycles=17304`。

`syn/` 要 Windows 上的 Efinity（`<EFINITY>` 见 `Main/doc/开发环境.md` 第 1 节）：

```
cd Main\sim\engine\pix\syn
call <EFINITY>\bin\setup.bat
efx_run.bat probe_pix_syn.xml
efx_run.bat engine_pix_syn.xml
```

## 结论速查

- 行内像素号 `j = 4k + t − ld`（k 目的拍号、t 槽号、`ld` 目的行首相位），有效 ⟺ `0 ≤ j < npx`。
- 旋转：`off = (ls ≥ ld)`，窗口 {前拍, 当前拍}，起始槽 `st = (ls − ld) mod 4`；`off = 1` 时行首先预热一拍（先发地址 0）。
- 流水 7 拍（行缓冲读出 → 旋转/色键 → 混合核 L=3 → 标记合并），下游 ready 时行内无气泡；色键、掩码、拍号、valid 的延迟像逐级贴着数据链。
- 写核每个写描述符的首拍必停一拍、输出 FIFO 半满也停，整条流水挂 `en = ws_tready || !ws_tvalid`。
- 行缓冲 RAM：源 512×128 7 块、目的 512×96 5 块（X 字节不存），合计 12 块。
- FILL 的 a 一律取 `w4[31:24]`（规范条款 §4.2），不看 `sfmt`。
- 写流的 `tlast` 由搬运层按写描述符拍数打，本层不出 `tlast`。
- 引擎数据通路自己不挡跨命令的写后读：下一条命令要等上一条写回，或由控制层的命令下发另行处理（`path_hazard_nowait` 必须见红）。
