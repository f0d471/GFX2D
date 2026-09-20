/******************************************************************************
* @file test_gfx.h: 构造层与软件执行器的主机侧测试入口
*
* @brief  由 test_main.c 调用，返回失败项数。
*         vecpath 为 NULL 时跳过导出向量那一组（它要先跑一次
*         tools/check/check_cmd_semantics.py --emit）；gen 非 0 时
*         把整帧用例的 CRC 写进 golden 而不是拿来比对。
******************************************************************************/
#ifndef GFX2D_TEST_GFX_H
#define GFX2D_TEST_GFX_H

#include <stdint.h>

uint32_t test_gfx_run(const char *vecpath, const char *golden, int gen);

#endif /* GFX2D_TEST_GFX_H */
