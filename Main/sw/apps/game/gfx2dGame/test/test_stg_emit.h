/******************************************************************************
* @file test_stg_emit.h: 弹种表、发射器与五种弹型（sw 第 06 章 3.7 节）的主机侧测试入口
*
* @brief  test_stg_emit_run 返回失败项数；轨迹与 dump 给 check_sw7_game.sh 比期望文件用。
******************************************************************************/
#ifndef TEST_STG_EMIT_H
#define TEST_STG_EMIT_H

#include <stdint.h>

uint32_t test_stg_emit_run(void);

/* 六段各 120 帧的轨迹，格式见 test_stg_emit.c 的文件头。 */
void     stg_pat_trace(void);

/* 第 seg 段第 frame 帧（从 0 起）之后的全部敌弹。参数越界返回 -1。 */
int      stg_pat_dump(uint32_t seg, uint32_t frame);

#endif /* TEST_STG_EMIT_H */
