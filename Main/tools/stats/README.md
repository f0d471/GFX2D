# stats：统计日志出图

SW8 统计埋点（[`../../../Engineering/TechReports/sw/07-演示模式与统计埋点.md`](../../../Engineering/TechReports/sw/07-演示模式与统计埋点.md) 3.9、3.11 节）
的出图工具。输入是 `gfx2dGame` 模式框架打出来的日志，输出是报告与幻灯片用的图的源码。

| 文件 | 用途 |
| --- | --- |
| `plot_stats.py` | 读日志里的 `csvh`、`csv`、`csvsum` 与 `nmax` 几种行，出三张图的 HTML+SVG 源码：按混合方程的像素占比、帧时间分布、`N_max` 搜索轨迹 |

只用标准库（WSL 的 python3 没有 matplotlib）。PC 的标准输出与板上 `board_run.py` 录下的串口日志都能读：
行尾的 `\r`、`#` 开头的标记行与其它日志行都跳过。

## 用法

```
wsl -e python3 Main/tools/stats/plot_stats.py --out Deliverables/Assert/figures/src [--prefix sw8_] [--ref-s1] \
    [--nmax <搜索日志>] <日志>[=<标签>[@<跳过行数>]] ...
bash Deliverables/Assert/figures/src/render.sh sw8_pixel_share sw8_frame_time sw8_nmax_search
```

| 参数 | 说明 |
| --- | --- |
| `<日志>=<标签>` | 位置参数的日志出前两张图。一份日志里有几个模式就拆成几行，模式 1 按抽样帧的 `n_active` 再拆成场景 A 与场景 B；帧时间图最多画前四个场景 |
| `@<跳过行数>` | 每个场景丢掉开头这么多行 `csv`（负载还在爬升的那几秒） |
| `--nmax <日志>` | 只取它的 `nmax` 行出搜索轨迹图，可给多次。模式 3 的搜索在扫负载，它的 `csv` 行不该进前两张图的平均 |
| `--ref-s1` | 像素占比图最后加一行设计文档 4.5 节 S1 的估算，标「估算」；数字写在脚本的 `REF_S1` 里，改设计文档那张表时同步改 |
| `--hz` | 板子的时钟频率，缺省 100 MHz；一场 F = hz / 60，日志里有 `nmax cfg` 时以它的 `F=` 为准 |

标准输出打出每张图用到的数（各场景的方程像素均值、每秒 `ft_max` 的分布、每遍搜索的逐档旋钮与结果），
报告与开发日志引用这些数，不从图上读。三张图都出了退出码为 0；缺某种行时对应的图不出，打一行说明，退出码 1。

入库的三张图（`Deliverables/Assert/figures/sw8_*.png`）由 `gfx2dGame/logs/` 下的四份样例日志按下面这组参数画出；
`check_sw8_stats.sh` 的 M17 用同一组参数重出一遍，与入库的图源逐字节比：

```
L=Main/sw/apps/game/gfx2dGame/logs
wsl -e python3 Main/tools/stats/plot_stats.py --out Deliverables/Assert/figures/src --ref-s1 \
    --nmax $L/sw8_stats_press.log \
    "$L/sw8_stats_demo.log=基础 Demo" "$L/sw8_stats_game.log=游戏（自动驾驶）" \
    "$L/sw8_stats_s1.log=S1 规模（模式 3，旋钮 1744）@10"
```

四份样例日志怎么录的（构建开关、帧数）见 sw 第 07 章 3.11 节。PC 日志里的帧时间是墙钟，只验证流程；
板上的数字由 SW9 用同一个脚本出图。

## 画法约定

照 [`../../../Deliverables/Assert/README.md`](../../../Deliverables/Assert/README.md)「画图约定」：底色 `#FAFAF7`，1760×880，
由 `render.sh` 以 2 倍像素导出。方程的颜色与 `profile.html` 一致：COPY 蓝 `#4A7FD0`、ALPHA 绿 `#2E7D4F`、ADD 金 `#A37A1C`、CONST 红 `#E8546B`。
这组颜色在 `#FAFAF7` 上金与绿相邻那一对的色盲区分度在 6~8 之间，所以堆叠条的每段都带直接标注（放得下时）与 2 像素间隙，图例常在。
搜索轨迹图里通过与掉帧除了颜色还靠形状区分（实心点、空心叉）。
