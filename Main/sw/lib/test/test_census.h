/******************************************************************************
* @file test_census.h: 按方程的普查（sw 第 07 章 3.2 节）的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
******************************************************************************/
#ifndef GFX2D_TEST_CENSUS_H
#define GFX2D_TEST_CENSUS_H

#include <stdint.h>

uint32_t test_census_run(void);

#endif /* GFX2D_TEST_CENSUS_H */
