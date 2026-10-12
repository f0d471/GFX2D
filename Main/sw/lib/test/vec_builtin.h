/******************************************************************************
* @file vec_builtin.h: C 侧手写的一致性向量
*
* @brief  输入与期望都按命令语义的条文直接写死，不看
*         `check_cmd_semantics.py` 里的常量，也不调用任何执行器来算期望。
*         它与工具导出的那 159 条是两个来源：两边都过，才排除得掉
*         「向量本身与实现共享同一个错误」。
*
*         这一组不依赖 python，`make test` 默认就跑。
******************************************************************************/
#ifndef GFX2D_TEST_VEC_BUILTIN_H
#define GFX2D_TEST_VEC_BUILTIN_H

#include "vec_replay.h"

uint32_t vec_builtin_count(void);

/* 填好第 idx 条向量，idx 越界时返回 0。 */
int vec_builtin_get(uint32_t idx, vec_case *v);

#endif /* GFX2D_TEST_VEC_BUILTIN_H */
