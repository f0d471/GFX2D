# 可选机制

本目录收录两类内容：前期研究的归档证据，以及正式开发的构型验证脚本。

## 正式开发的构型脚本

| 脚本 | 判据 |
| --- | --- |
| [elab_all.sh](elab_all.sh) | 七个代表档各生成一棵构型源树并 elaboration 通过 |
| [elab_legal.sh](elab_legal.sh) | 16 种合法结构组合逐个 elaboration 通过；16 种非法组合逐个被构建入口拒绝 |
| [elab_pixel.sh](elab_pixel.sh) | 八种像素位组合逐个 elaboration 通过，并打印各自的合法方程集 |
| [elab_tree.sh](elab_tree.sh) | 在指定构型源树里编 `engine_top`（被上面两个调用） |
| [elab_verbose.sh](elab_verbose.sh) | 同上但打印完整编译器输出，诊断用 |
| [probe_cut.sh](probe_cut.sh) | 对预处理后的源文件数实例化点，断言关闭的机制离开编译输入 |
| [probe_strip.sh](probe_strip.sh) | STRIP 不增减例化，改用源码内容判定：断言条带状态与条带表只在 STRIP 开时出现 |
| [guard_negative.sh](guard_negative.sh) | 五条非法构型被构建入口拒绝且诊断指认到具体依赖；研究构型被接受 |
| [regress_suites.sh](regress_suites.sh) | 批量跑指定仿成套件并汇总末行结果 |
| [tb_feature_elab.v](tb_feature_elab.v) | elaboration 探针，只验通道接通，不设功能判据 |

构型真源与构建入口：

- [feature_config.py](../../../tools/check/feature_config.py)：位定义、依赖、合法性与 CAPS 计算，
  导入时断言 16 合法结构 / 128 研究 / 32 交付。
- [feature_build.py](../../../tools/check/feature_build.py)：按构型生成特性头、CAPS 常量、
  软件能力头与工程源列表；`--sync` 把默认全功能构型的特性头写进 `rtl/engine/`，
  `--check` 校验它与默认构型一致。

WSL 中从仓库根执行：

```sh
python3 Main/tools/check/feature_build.py --list
python3 Main/tools/check/feature_build.py --check
bash Main/sim/engine/features/elab_all.sh
bash Main/sim/engine/features/probe_cut.sh
bash Main/sim/engine/features/guard_negative.sh
```

## 前期研究归档

| 内容 | 入口 |
| --- | --- |
| 2026-10-15 研究归档 | [logs/20261015/](logs/20261015/README.md) |
| 可复用研究工具 | [research_engine_features.py](../../../tools/check/research_engine_features.py) |
| 现行检查范围 | [验证流程](../../../doc/验证流程.md) |

前期研究的归档是历史运行快照，按四结构位那套输入跑的；
构型定义已由 `feature_config.py` 改为五结构位，两者不要混用。

```sh
python3 Main/tools/check/validate.py --checks feature-research --profile quick --run
```

需要隔离 map 或明确基线板测时，先运行模型并记下 RUN 目录，再使用同一输入续跑：

```sh
python3 Main/tools/check/research_engine_features.py --profile quick --models --prepare-synthesis --blend-models
```

Windows Efinity Python 环境（按开发环境设置 PYTHONHOME）中运行：

```text
python Main/tools/check/research_engine_features.py --profile quick --run-dir <RUN> --synthesis
python Main/tools/check/research_engine_features.py --profile quick --run-dir <RUN> --board --bit <冻结位流>
python Main/tools/check/research_engine_features.py --profile quick --run-dir <RUN> --collect
```

同一运行目录的动作顺序执行；板测每次实际执行。
`--collect` 要求四项本轮证据齐备，拒绝把仅模型通过汇总成完整研究通过。
原始输出进 `sim/build/check-runs`，不覆盖历史证据；研究归档与原位流版本绑定。
