/******************************************************************************
* @file test_blit_run.h: 批量提交 gfx_blit_run()（sw 第 06 章 3.6 节）的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
******************************************************************************/
#ifndef GFX2D_TEST_BLIT_RUN_H
#define GFX2D_TEST_BLIT_RUN_H

#include <stdint.h>

uint32_t test_blit_run_run(void);

#endif /* GFX2D_TEST_BLIT_RUN_H */
