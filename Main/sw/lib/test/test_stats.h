/******************************************************************************
* @file test_stats.h: 统计基础件（帧时间直方图与日志队列）的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
******************************************************************************/
#ifndef GFX2D_TEST_STATS_H
#define GFX2D_TEST_STATS_H

#include <stdint.h>

uint32_t test_stats_run(void);

#endif /* GFX2D_TEST_STATS_H */
