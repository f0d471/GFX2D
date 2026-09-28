/******************************************************************************
* @file test_stg_game.h: 自机与规则（sw 第 06 章 3.10 节，C 批）的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
******************************************************************************/
#ifndef TEST_STG_GAME_H
#define TEST_STG_GAME_H

#include <stdint.h>

uint32_t test_stg_game_run(void);

#endif /* TEST_STG_GAME_H */
