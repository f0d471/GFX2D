/******************************************************************************
* @file rast_ref.h: setup 记录的增量软件执行器
* @brief 颜色与深度均以条带局部 y 寻址；stride 单位为像素/深度元素。
******************************************************************************/
#ifndef GFX2D_GFX_RAST_REF_H
#define GFX2D_GFX_RAST_REF_H
#include "gfx/rast_setup.h"

typedef struct {
    uint32_t *color;
    uint16_t *depth;
    unsigned stride, strip;
    const uint32_t *texture;
    unsigned tex_width, tex_height, tex_stride;
    uint32_t key;
} rast_target;

/* 先检查整批参数与记录，错误返回 -1 且不写目标。返回 0 表示执行完成。
   clear 属于调用者：depth=65535；颜色由条带背景决定。 */
int rast_ref_run(const rast_record *records, unsigned count, const rast_target *target);
#endif
