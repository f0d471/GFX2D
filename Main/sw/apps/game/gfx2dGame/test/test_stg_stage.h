/******************************************************************************
* @file test_stg_stage.h: 敌机、Boss 与关卡（sw 第 06 章 3.11 节，C 批）的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
******************************************************************************/
#ifndef TEST_STG_STAGE_H
#define TEST_STG_STAGE_H

#include <stdint.h>

uint32_t test_stg_stage_run(void);

#endif /* TEST_STG_STAGE_H */
