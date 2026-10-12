/******************************************************************************
* @file test_stg_auto.h: 自动驾驶的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
******************************************************************************/
#ifndef TEST_STG_AUTO_H
#define TEST_STG_AUTO_H

#include <stdint.h>

uint32_t test_stg_auto_run(void);

#endif /* TEST_STG_AUTO_H */
