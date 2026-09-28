/******************************************************************************
* @file test_stg_pool.h: 实体池（sw 第 06 章 3.5 节）的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
******************************************************************************/
#ifndef TEST_STG_POOL_H
#define TEST_STG_POOL_H

#include <stdint.h>

uint32_t test_stg_pool_run(void);

#endif /* TEST_STG_POOL_H */
