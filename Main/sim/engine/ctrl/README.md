# engine/ctrl：控制层仿真

控制层已于 2026-10-02 验收，6576 项检查 0 错。

| 文件 | 用途 |
| --- | --- |
| `probe_check.v` | 十步检查三种结构的研究探针（`MODE` 0 单拍组合 / 1 多拍状态机 / 2 SURF 预计算），行为权威是 `sw/lib/gfx/check.c` 的 `gfx_cmd_check()`。含 efx_map 2026.1 崩溃的绕行写法（逐槽预测）与注释 |
| `tb_probe_check.v` | 测试台，三路判据：定向 54 条（期望按 check.c 手推，文件头有位号备忘）、随机 3000 组三结构互比、结构 2 预计算位逐拍等价监视 |
| `run.sh` | 仿真入口，`wsl -e bash -c 'cd Main && bash sim/engine/ctrl/run.sh'`；研究探针、159 向量与定向用例、两项注错，全部符合预期才返回 0 |
| `tb_engine_top.v` | `engine_top` 全链 APB 端到端仿真；159 条一致性向量与取数边界/总线错误/超时/软复位/写后读/首错顺序/PERF 对账的定向判据，验收时 6576 项检查 0 错 |
| `syn/` | 三个研究探针综合工程及 `engine_top_syn.xml` 正式控制层单独综合工程；均用 100 MHz 四步流程，验收数据见 `syn/` 各工程报告 |

研究探针不是产品 RTL；`rtl/engine/engine_top.v`、`engine_regs.v`、`engine_cmd.v`
是控制层的入库实现。本目录保留探针作为回归。引擎接入 SoC 与引擎自身上板验收
见 `engine/frame` 与 `rtl/README.md`。
