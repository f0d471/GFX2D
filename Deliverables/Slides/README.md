# Slides

项目讲解幻灯片，用 Claude Design（claude.ai 的幻灯片 Artifact）制作。

## 状态

| 文件 | 内容 | 状态 |
| --- | --- | --- |
| 在线幻灯片「gfx2d 项目讲解」 | 22 页，每页一张主图：赛题、板卡、工程构建流程、当前架构、启动、CPU 访存、MMIO、AXI4、状态机、HDMI、帧缓冲、设计方法、拆账、Alpha 混合、像素流水线、Roofline、DMA、条带合成、目标硬件架构、目标软件与验证、计划 | 第二版（2026-09-18） |
| `gfx2d项目讲解-20260918.pdf` | 在线幻灯片的 PDF 版，22 页，1920×1080，不含讲稿备注 | 与在线第二版同页序 |
| `print.html` | 生成上面 PDF 的源文件，引用 `../Assert/` 下的图 | 改图后用 Edge 无头 `--print-to-pdf` 重新导出 |

每页的图来自 [`../Assert/figures/`](../Assert/figures/)，改图时改 `figures/src/` 下的源码再导出、重新上传。
导出命令：`msedge --headless=new --no-pdf-header-footer --print-to-pdf=<输出.pdf> file:///<本目录>/print.html`。导出的文件放本目录，文件名带日期。

## 引用的数字

| 数字 | 出处 | 性质 |
| --- | --- | --- |
| 探针 ID `0x47465832`、640×480@60、25.2 MHz | platform 第 01 章 | 实测 |
| 突发上限 256 拍，`t_tx` 读 27.41 / 写 7.25，`k` 读 1.086 / 写 1.183 | platform 第 03 章 | 实测 |
| 数据缓存结构、无写缓冲、失效开销 | platform 第 02、05 章 | 实测 |
| 各突发长度的有效带宽、ALPHA 工作点 | 由上一行拟合式推算 | 推算 |
| XLR 16151、片上 RAM 65 块、DSP 4 个 | platform 第 03、04 章 | 实测 |
| 除以 255 的舍入式与位宽 | spec 第 01 章、`Main/doc/赛题二总体设计.md` 3.5 节 | 穷举校验 |
| 场景 S1 的 Sprite 数与像素数、每 Sprite 事务数、周期预算、片上 RAM 预算 173 块 | `Main/doc/赛题二总体设计.md` 2.3、3.1、4.5 节 | 估算 |

上述出处的数字被订正时，对应的图要一起改。
