# G3 条带控制/分桶的研究工件

前期研究的脚本、原型与原始输出。生产 RTL、软件库与本目录互不引用；
正式判据与结论归
[engine 第 09 章](../../../../Engineering/TechReports/engine/09-条带控制与ABIv2.md)，
条目在 `Engineering/Todo/hw/引擎P3-G3条带控制与ABIv2.md`。

## 分桶分布与取数模型

仓库根运行（PC 或 WSL，需 Python 3）：

```text
python3 Main/tools/check/g3_research.py --json
```

按 `stg_draw.c` 的几何口径重放一帧（背景全屏 FILL＋15 横线＋13 竖线＋60 星点、
N 颗均匀弹、HUD 近似），输出条带 16/32 行的桶分布、峰值桶对 D61 区容量
1024 的余量、均匀假设下的溢出边界弹数，以及直通/条带的取数批与模型周期。
K896/K1055 峰值桶 ≤1024 才输出 PASS。摘要落 `logs/research_results.json`。

## 等价性模糊原型

`tiling_fuzz.c` 用生产 lib（`check/list/exec_sw`）原样编译，分桶器与条带
执行循环在本文件内：同一批绘制调用，直通（平表＋SET_CLIP）与条带
（clip 吸收＋按条带分桶＋逐桶 BAND 影子执行）的最终帧逐字节比对；
4 个定向变异（漏桶、桶内序倒置、clip 吸收偏移、BAND 不写回）必须见红。

WSL 中从仓库根：

```text
gcc -std=c11 -I Main/sw/lib -fsanitize=address,undefined -g \
    Main/sim/engine/stripctl/tiling_fuzz.c \
    Main/sw/lib/gfx/check.c Main/sw/lib/gfx/list.c Main/sw/lib/gfx/exec_sw.c \
    Main/sw/lib/core/format.c -o Main/sim/engine/stripctl/build/tiling_fuzz
Main/sim/engine/stripctl/build/tiling_fuzz --json
```

1000 张随机场景逐字节一致且 4 变异见红输出 PASS；摘要落
`logs/fuzz_results.json`。环境开关只作调试：`FUZZ_ONE` 固定单条跨桶 BLIT、
`FUZZ_NOCLIP` 关掉 clip 场景、`FUZZ_DUMP` 转储两侧命令序列。
原型规模 1000 张；正式批 ≥1 万张与错误三元组对账归 G3 实现。

## 证据索引

| 文件 | 内容 |
| --- | --- |
| `logs/research_results.json` | 桶分布、峰值/溢出边界、取数批与模型周期 |
| `logs/fuzz_results.json` | 场景数、逐字节一致数、4 变异见红 |
| `tiling_fuzz.c` | 分桶器＋条带执行循环原型（不动生产源码） |

研究不修改生产 RTL、软件库与 XML；构建产物在忽略的 `build/`。
