/******************************************************************************
* @file stg_pattern.h: B 批的五种弹型与六段
*
* @brief  sw 第 06 章 3.7 节的五组参数。五种弹型各是一条常量发射记录加发射器的起点与宿主运动；
*         第 5 段「合并」把五个发射器按段号顺序放进同一个场。
*         弹型判据的 Python 独立实现（tools/check/gen_stg_patterns.py）另抄一份参数，
*         判据先比两边打出来的参数行，抄错时报「参数不一致」。
******************************************************************************/
#ifndef STG_PATTERN_H
#define STG_PATTERN_H

#include <stdint.h>

#include "stg_emit.h"
#include "stg_field.h"

#define STG_PAT_N       5u              /* 五种弹型：段 0~4 */
#define STG_SEG_MIX     5u              /* 第 5 段：合并 */
#define STG_SEG_N       6u
#define STG_PAT_SEED    0x5E7B0001u     /* 每段的场都用这个种子 */

typedef struct {
    const char   *name;
    stg_emit_rec  rec;
    int32_t       x, y;                 /* 发射器起点，Q.8 */
    stg_host      host;                 /* 替身宿主的运动 */
} stg_pattern;

extern const stg_pattern stg_patterns[STG_PAT_N];

/* 段名：ring、spiral、aim、fan、rain、mix；越界返回 "?"。 */
const char *stg_seg_name(uint32_t seg);

/* 把 f 初始化成第 seg 段的起点：空场（种子 STG_PAT_SEED）加这一段的发射器。
   seg 越界或挂发射器失败返回 -1。 */
int stg_pattern_load(stg_field *f, uint32_t seg);

#endif /* STG_PATTERN_H */
