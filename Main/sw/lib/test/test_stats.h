/******************************************************************************
* @file test_stats.h: 统计基础件（sw 第 07 章 3.3、3.4 节）的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
******************************************************************************/
#ifndef GFX2D_TEST_STATS_H
#define GFX2D_TEST_STATS_H

#include <stdint.h>

uint32_t test_stats_run(void);

#endif /* GFX2D_TEST_STATS_H */
