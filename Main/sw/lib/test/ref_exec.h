/******************************************************************************
* @file ref_exec.h: 软件执行器的独立参考实现（只给主机侧测试用）
*
* @brief  `lib/gfx/exec_sw.c` 是产品实现，这里再写一份不追求速度、
*         照规范伪代码逐条抄的版本，两者的输出必须逐项相同。
*         独立的理由是两条教训：一个实现自己判自己不算证据，
*         两个实现共享同一个错误同样不算。
*
*         「独立」落到三处：自己解命令的位域，不用 gfx_abi.h 的取字段函数；
*         自己写一份十步校验，不调用 gfx_cmd_check()；
*         除以 255 用另一个算式（(x + 127) / 255），不用移位近似式。
*         两份实现共享的只有 ABI 常量与表面表的内存布局。
*
*         本文件只在 `sw/lib/test/` 里使用，不进 lib 的库列表。
******************************************************************************/
#ifndef GFX2D_TEST_REF_EXEC_H
#define GFX2D_TEST_REF_EXEC_H

#include "gfx/gfx.h"

/* clip 由调用方持有，语义与 exec_sw_state 的那一份相同：
   持续到下一条 SET_CLIP 或软复位，不随命令表或帧自动复位。 */
void ref_exec_reset(gfx_rect *clip);

int  ref_exec_run(gfx_rect *clip, const gfx_surface_table *t, const gfx_mem *m,
                  const gfx_cmd *list, uint32_t list_count, uint32_t readable_count,
                  exec_sw_stats *st, exec_sw_error *err);

#endif /* GFX2D_TEST_REF_EXEC_H */
