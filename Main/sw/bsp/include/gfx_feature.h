/* 由 tools/check/feature_build.py 生成：本构型的编译时预期能力。
 * 用于把交付物与硬件构型对上；运行时探测仍以 CAPS 寄存器为准。
 * 能力位表示这块硬件具备什么，不表示默认走哪条后端路径——
 * 本构型 11111-111-d 的默认后端由 renderer 的 GFX_FRONTEND_* 决定，与能力位无关。 */
#ifndef GFX_FEATURE_H
#define GFX_FEATURE_H

#define GFX_FEATURE_CONFIG   "11111-111-d"
#define GFX_FEATURE_CAPS     0x0000346Fu
#define GFX_FEATURE_TEX   1
#define GFX_FEATURE_BAND_HW   1
#define GFX_FEATURE_STRIP   1
#define GFX_FEATURE_INST   1
#define GFX_FEATURE_PB   1
#define GFX_FEATURE_EQ_EXT   1
#define GFX_FEATURE_ALPHA   1
#define GFX_FEATURE_KEY   1
#define GFX_FEATURE_DELIVERY   1

/* 编译时预期与运行时 CAPS 的一致性：预期为 1 的能力位必须真的为 1，
 * 且设备不得报告本构型没有的能力位。 */
#define GFX_FEATURE_CAPS_MATCH(caps) \
    ((((caps) & GFX_FEATURE_CAPS) == GFX_FEATURE_CAPS) && \
     (((caps) & ~(GFX_FEATURE_CAPS)) == 0u))

#endif
