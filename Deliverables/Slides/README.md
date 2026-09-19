# Slides

项目讲解幻灯片，22 页，每页一张主图。第二版最早用 Claude Design（claude.ai 的幻灯片 Artifact）制作，
现在以本目录的 PPTX 为准。

## 状态

| 文件 | 内容 | 状态 |
| --- | --- | --- |
| `gfx2d项目讲解.pptx` | 22 页，16:9。封面用原生形状画，内容页是页标题加一张主图，不带演讲者备注 | 第三版（2026-09-20） |
| `gfx2d项目讲解-20260920.pdf` | PDF 版，22 页，1920×1080 | 与 PPTX 同页序、同封面 |
| `build_pptx.js` | 生成 PPTX 的脚本：页序与页标题写在脚本里，图取自 `../Assert/figures/`；命令行给出讲稿路径时把讲稿写进备注 | |
| `print.html` | 生成 PDF 的源文件，引用 `../Assert/` 下的图 | |
| 在线幻灯片「gfx2d 项目讲解」 | Claude Design 上的第二版 | 停在第二版，封面与第 4、13 页未同步 |

页序：封面、赛题、板卡、工程构建流程、当前架构、启动、CPU 访存、MMIO、AXI4、状态机、HDMI、帧缓冲、
设计方法、拆账、Alpha 混合、像素流水线、Roofline、DMA、条带合成、目标硬件架构、目标软件与验证、计划。

## 重新生成

每页的图来自 [`../Assert/figures/`](../Assert/figures/)，改图时改 `figures/src/` 下的源码再导出。
改了图之后，PPTX 与 PDF 都要重新生成：

```
npm i pptxgenjs                     在任意临时目录装一次，不入库
NODE_PATH=<临时目录>/node_modules node build_pptx.js [讲稿.md]
msedge --headless=new --no-pdf-header-footer --print-to-pdf=<本目录>/gfx2d项目讲解-<日期>.pdf file:///<本目录>/print.html
```

讲稿不入库。给出讲稿路径时按 `## 第 N 页` 切分写进备注，缺页会报错退出；入库的 PPTX 不带讲稿。
改页序时 `build_pptx.js` 的 `PAGES` 与 `print.html` 一起改。PDF 文件名带导出日期，新文件替换旧文件。

## 引用的数字

| 数字 | 出处 | 性质 |
| --- | --- | --- |
| 探针 ID `0x47465832`、640×480@60、25.2 MHz | platform 第 01 章 | 实测 |
| 突发上限 256 拍，`t_tx` 读 27.41 / 写 7.25，`k` 读 1.086 / 写 1.183 | platform 第 03 章 | 实测 |
| 数据缓存结构、无写缓冲、失效开销 | platform 第 02、05 章 | 实测 |
| 各突发长度的有效带宽、ALPHA 工作点 | 由上一行拟合式推算 | 推算 |
| XLR 16151、片上 RAM 65 块、DSP 4 个 | platform 第 03、04 章 | 实测 |
| 工具链 Efinity 2026.1、riscv-none-elf-gcc 13.4 | platform 第 07 章 | 实测 |
| 除以 255 的舍入式与位宽 | spec 第 01 章、`Main/doc/赛题二总体设计.md` 3.5 节 | 穷举校验 |
| 场景 S1 的 Sprite 数与像素数、每 Sprite 事务数、周期预算、片上 RAM 预算 173 块 | `Main/doc/赛题二总体设计.md` 2.3、3.1、4.5 节 | 估算 |
| 每弹幕约 100 条指令、5000 弹幕约 5 ms | `Main/doc/赛题二总体设计.md` 3.1 节 | 估算 |

上述出处的数字被订正时，对应的图要一起改。
