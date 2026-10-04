# Report

技术报告。源文件 `parts/*.md`，构建脚本 `build_report_docx.py`，成品 `gfx2d技术报告.docx`。
格式基准：官方模板 `报告模板.docx`（五部分骨架、A4、上下边距 2.54 cm、左右 3.175 cm、
正文 12 pt）。模板是下限，成文质量按学术报告标准执行。

## 章节结构（模板骨架 ← 内容映射）

| 模板部分 | 内容 | 主要取材 |
| --- | --- | --- |
| 作品名称 + 摘要 | 作品定位与关键指标 | 数据台账 |
| 第一部分 作品概述 | 1.1 功能与特性 / 1.2 应用领域 / 1.3 技术特点 / 1.4 性能指标(表) / 1.5 创新点 / 1.6 设计流程 | 赛题原文、总体设计 0 节 |
| 第二部分 系统组成及功能说明 | 2.1 整体介绍 / 2.2 硬件系统（平台与地址空间、总线接入与仲裁、扫描输出与双缓冲、渲染引擎与像素流水线）/ 2.3 软件系统（分层与双目标、命令构造与软件执行器、素材流水线、游戏与演示模式）/ 2.4 验证体系 | 总体设计 2~4 节、platform 02~08、display 01、engine 01~05、spec 01~02、sw 01~07 |
| 第三部分 完成情况及性能参数 | 3.1 整体介绍(实拍) / 3.2 工程成果（资源与时序、软件成果）/ 3.3 特性成果（帧率对比、N_max、稳态、要求对照） | engine 06、sw 08、数据台账 |
| 第四部分 总结 | 4.1 可扩展之处 / 4.2 心得体会 | — |
| 第五部分 参考文献 | ≤20 篇，标准格式 | Assert/third-party 台账 |
| 附：AI 工具使用说明 | 仅辅助性工作（资料整理、文字校对、图表美化） | — |

## 构建方法

```
%LOCALAPPDATA%\loongarch-report-py312\python.exe build_report_docx.py
```

依赖该目录缓存的 python-docx / PIL / matplotlib / fitz（随龙芯项目部署，离线可用）。
PDF 预览与验收：LibreOfficePortable（`E:\Software`）headless 转 PDF，
fitz 按 100 dpi 出 PNG 逐页目视。文档无 Word COM，不使用目录域；
正文标题用内置 Heading 样式，便于评审方在自己的 Word/WPS 里自行生成目录。

## 引用登记

报告引用的每个数字登记在 [`数据台账.md`](数据台账.md)，含出处章节与日志文件；屏摄与 PC
截图画面中的读数也登记在该文件列表。配图源码在
[`../Assert/figures/src/report/`](../Assert/figures/src/report/)，
导出图在 [`../Assert/figures/report/`](../Assert/figures/report/)；
数据图由 `src/report/plot_report_*.py` 从 SW9 日志生成，不手改。
