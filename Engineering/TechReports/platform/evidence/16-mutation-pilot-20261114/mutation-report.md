# 变异测试报告

运行目录：`Main/sim/build/check-runs/mutation-20261114-012027`

基线：8/8 通过

| 变异 | 缺陷模型 | 用例 | 结果 | 见红位置 | 状态 |
| --- | --- | --- | --- | --- | --- |
| blend.copy_dst | 输入依赖 | narrow_copy | FAIL | MISMATCH #0: eq=0  | KILLED |
| blend.wrong_eq | 运算恒等 | narrow_const | FAIL | MISMATCH #1: eq=3  | KILLED |
| blend.unreg_c | 流水对齐 | narrow_unreg | FAIL | MISMATCH #65791: eq=1  | KILLED |
| blend.x_no_en | 停顿时序 | stall | FAIL | MISMATCH #1: eq= | KILLED |
| blend.trunc16 | 位宽截断 | narrow_trunc | FAIL | MISMATCH #767: eq=2  | KILLED |
| pix.en_notready | 握手反压 | unit_stall | FAIL | BEATS_OR_GAP n= | KILLED |
|  |  | pipe_burst | FAIL | PIX_MISMATCH beat= |  |
| pix.core_noen | 停顿时序 | unit_stall | FAIL | PIX_MISMATCH n= | KILLED |
|  |  | pipe_burst | FAIL | PIX_MISMATCH beat= |  |
| pix.rot_off | 像素相位 | unit_stall | FAIL | PIX_MISMATCH n= | KILLED |
| pix.skip_key | 条件判断 | unit_stall | FAIL | MASK_MISMATCH n= | KILLED |
|  |  | path_golden | FAIL | MEM_MISMATCH addr= |  |
| pix.drop_mask | 掩码边界 | unit_stall | FAIL | MASK_MISMATCH n=1 ls=0 ld=0 beat=0 slot=1 | KILLED |

合计 10 条：KILLED 10
