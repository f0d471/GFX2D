# F1～F3 实施研究记录

2026-10-10 开始核对生产路径。基线为提交 `1e2cd8d`，工作区已有
`Deliverables/Report/.work/`，本批不操作其中内容。

研究顺序：先用真实 `stg_game_compose` / `gfx_blit_run` 测 F1，随后分别实现
背景 pass 与 planned 分桶，再做组合。默认后端由同状态板测决定。

源码依据：`strip.c` 先检查所有覆盖桶再写入；`list.c` 的批量入口复用公共字段
检查，逐项保留坐标、关闭状态、容量和反馈检查；`renderer.c` 收尾上一在途帧后
才允许新提交；`stg_draw.c` 背景的底色、横线、竖线、星点顺序必须保持。

来源复核：NVIDIA GPU Gems 3 第 39 章的 exclusive scan 只用于构造紧凑引用
区间；单核 30 桶使用线性前缀和，不移植 CUDA 并行扫描。LVGL v8.3 Drawing
文档的完成通知与缓冲复用约束作为资源租约设计参考，不借用其像素算法。
Mesa Panfrost 文档用于区分绘制前端与像素执行阶段，不据此推导本机收益。

- <https://developer.nvidia.com/gpugems/gpugems3/part-vi-gpu-computing/chapter-39-parallel-prefix-sum-scan-cuda>
- <https://docs.lvgl.io/8.3/overview/drawing.html>
- <https://docs.mesa3d.org/drivers/panfrost.html>

板卡当前扫描结果：FT4232H，Ti60 IDCODE `0x10660a79`，IR 宽度 5，UART COM11。
新增验证使用 JTAG 与 DDR 程序装载；具体成功范围以本批原始日志为准。
