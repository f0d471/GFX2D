/******************************************************************************
* @file stg_pattern.c: 弹幕场的五种弹型与六段
*
* @brief  参数表就是下方这五组常量。坐标 = 像素 * FX_ONE，速度是 Q.8（256 = 1 像素/帧）。
*         改这里的任何一个数都要同步改 tools/check/gen_stg_patterns.py 与期望文件，
*         否则判据报「参数不一致」。
******************************************************************************/

#include "stg_pattern.h"

#include "stg_kind.h"

#define PX(v)   ((int32_t)(v) * FX_ONE)

const stg_pattern stg_patterns[STG_PAT_N] = {
    /* 0 圆环：32 发、ABS、一周均分；圆弹青 */
    { "ring",
      { .count = 32u, .dir_mode = STG_DIR_ABS, .kind = STG_KIND_ROUND16 + STG_COLOR_CYAN,
        .angle0 = 0u, .angle_step = 2048u, .spin = 0u, .interval = 20u, .repeat = 0u,
        .speed = 384, .speed_step = 0, .rand_x = 0, .rand_speed = 0 },
      PX(200), PX(160), { 0, 0, 0 } },

    /* 1 螺旋：3 发、SEQ、每帧转 1200；小圆弹品红 */
    { "spiral",
      { .count = 3u, .dir_mode = STG_DIR_SEQ, .kind = STG_KIND_SMALL8 + STG_COLOR_MAGENTA,
        .angle0 = 0u, .angle_step = 21845u, .spin = 1200u, .interval = 1u, .repeat = 0u,
        .speed = 512, .speed_step = 0, .rand_x = 0, .rand_speed = 0 },
      PX(200), PX(200), { 0, 0, 0 } },

    /* 2 自机狙：5 发、AIM、张开 8192，angle0 = -4096；宿主水平往返；米弹琥珀 */
    { "aim",
      { .count = 5u, .dir_mode = STG_DIR_AIM, .kind = STG_KIND_RICE20 + STG_COLOR_AMBER,
        .angle0 = 61440u, .angle_step = 2048u, .spin = 0u, .interval = 15u, .repeat = 0u,
        .speed = 768, .speed_step = 0, .rand_x = 0, .rand_speed = 0 },
      PX(200), PX(80), { 768, PX(80), PX(320) } },

    /* 3 扇形：7 发、ABS、以正下方为中心张开 6000，速度逐发加 64，共 6 次；米弹绿 */
    { "fan",
      { .count = 7u, .dir_mode = STG_DIR_ABS, .kind = STG_KIND_RICE20 + STG_COLOR_GREEN,
        .angle0 = 13384u, .angle_step = 1000u, .spin = 0u, .interval = 10u, .repeat = 6u,
        .speed = 640, .speed_step = 64, .rand_x = 0, .rand_speed = 0 },
      PX(200), PX(120), { 0, 0, 0 } },

    /* 4 随机雨：4 发、向下，出生点 x 随机 ±190 像素，速度随机加 0~2 像素/帧；小圆弹青 */
    { "rain",
      { .count = 4u, .dir_mode = STG_DIR_ABS, .kind = STG_KIND_SMALL8 + STG_COLOR_CYAN,
        .angle0 = 16384u, .angle_step = 0u, .spin = 0u, .interval = 3u, .repeat = 0u,
        .speed = 256, .speed_step = 0, .rand_x = 48640, .rand_speed = 512 },
      PX(200), PX(0), { 0, 0, 0 } },
};

const char *stg_seg_name(uint32_t seg)
{
    if (seg < STG_PAT_N)
        return stg_patterns[seg].name;
    if (seg == STG_SEG_MIX)
        return "mix";
    return "?";
}

int stg_pattern_load(stg_field *f, uint32_t seg)
{
    uint32_t i, lo, hi;

    if (seg >= STG_SEG_N)
        return -1;
    lo = (seg == STG_SEG_MIX) ? 0u : seg;
    hi = (seg == STG_SEG_MIX) ? STG_PAT_N : seg + 1u;

    stg_field_init(f, STG_PAT_SEED);
    for (i = lo; i < hi; i++) {
        const stg_pattern *p = &stg_patterns[i];

        if (stg_field_add_emitter(f, &p->rec, p->x, p->y, &p->host) != 0)
            return -1;
    }
    return 0;
}
