/******************************************************************************
* @file test_stg_draw.h: 按池发出与组帧（SW7 2.6 节 B 批）的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
******************************************************************************/
#ifndef TEST_STG_DRAW_H
#define TEST_STG_DRAW_H

#include <stdint.h>

uint32_t test_stg_draw_run(void);

#endif /* TEST_STG_DRAW_H */
