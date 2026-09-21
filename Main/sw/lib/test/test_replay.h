/******************************************************************************
* @file test_replay.h: 录像格式的主机侧测试
*
* @brief  见 test_replay.c 的文件头。判据是 Engineering/TechReports/sw/03-PC平台层与帧节拍.md 5.2 节的第 7 条。
******************************************************************************/
#ifndef GFX2D_TEST_REPLAY_H
#define GFX2D_TEST_REPLAY_H

#include <stdint.h>

/* 跑完这一组。fixture 是入库的 .g2dr 固件；同时比对由
   `Main/tools/check/replay2c.py` 生成的 C 数组（replay_fixture_bytes）。
   返回失败条数。 */
uint32_t test_replay_run(const char *fixture);

#endif /* GFX2D_TEST_REPLAY_H */
