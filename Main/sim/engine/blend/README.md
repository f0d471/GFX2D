# blend 单元验证

被测模块为 `../../../rtl/engine/core/engine_blend.v`，测试台固定使用三级流水。
需要 WSL 的 gcc、iverilog 与 vvp，不需要 Efinity 或板卡。

从仓库根使用统一入口：

```sh
python3 Main/tools/check/validate.py --checks blend --profile quick --run
```

`blend` 的 quick/acceptance/release 使用相同的完整参数：

- `check_round255.c`：舍入、饱和、运算顺序与预乘量化误差。
- `check_subst.c`：六式代入恒等、数值范围、截断反例与替代代入。
- `exhaustive`：六个方程有效操作数的完整域，共 50462976 个向量。
  其它端口随循环变量变化；这不是全部端口组合的穷举。
- `golden`：1024 条 C 程序导出的结果。
- `stall`：步长 5，427284 个向量，插入停顿并改变停顿期间的输入。

默认运行两项数学检查与三个 RTL 用例，规格失败、编译失败、进程非零退出、
结果缺失或多个结果标记都使脚本失败。产物写入本目录忽略的 `build/`，
统一入口的计划、输入身份和摘要写入本轮运行目录。

## 定向发现者

从本目录运行 `bash run.sh --case <名称>`。`spec` 只检查两项数学工具；
`golden` 准备其实际使用的 C 金标准；其它 RTL 单例不编译无关的 C 工具。

`narrow_copy` 覆盖 COPY 的 256 个值，`narrow_trunc` 覆盖 ADD 的 65536 组；
`narrow_unreg`、`narrow_const` 使用步长 17，包含 0、255，共 4096 组。
它们沿用相同参考式与检查器，供外部变异使用，不重复加入默认完整回归。
五条变异及其发现者登记在 `../../mutation/mutants.yaml`，通过统一入口运行：

```sh
python3 Main/tools/check/validate.py --checks mutation --profile quick --mutation-ids blend.copy_dst blend.trunc16 blend.unreg_c blend.wrong_eq blend.x_no_en --run
```

这条命令从仓库根执行。普通基线与变异在隔离树内运行，完整算术域由独立的 `blend` 检查覆盖。
