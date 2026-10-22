/* 独立第二执行器：逐采样点直接代入平面，不调用 rast_ref 的运算辅助函数。 */
#ifndef GFX2D_TEST_RAST_REF2_H
#define GFX2D_TEST_RAST_REF2_H
#include "gfx/rast_ref.h"
int rast_ref2_run(const rast_record *records, unsigned count, const rast_target *target);
#endif
