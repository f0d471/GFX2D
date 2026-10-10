/* F4 研究原型的共享核心：v1-I 实例包构造器与独立标量展开器。
 * 宿主对拍器（f4_inst_proto.c）与板测应用（apps/probe/f4Bench）编同一份
 * 实现，保证两侧成本与正确性结论指向同一段代码；生产实现由开发轮按
 * Todo/绘制前端F4 的 spec 另行交付，本文件不是候选 API。 */
#ifndef F4_INST_CORE_H
#define F4_INST_CORE_H

#include "../../../../lib/gfx/gfx_abi.h"
#include <stdint.h>

#define F4_OP_BLIT_INST 5u
#define F4_LAYOUT_XY4   0u
#define F4_LAYOUT_XYUV8 1u
#define F4_INST_MAX     65535u
#define F4_NO_OPEN      0xFFFFFFFFu

typedef struct {
    gfx_cmd *stream;      /* v1-I 包流，非实例命令原编码透传 */
    uint32_t used, cap;
    uint8_t *arena;       /* 实例数组，紧凑 stride，entries 按包顺序连续 */
    uint32_t arena_used, arena_bytes;
    uint32_t open;        /* 打开中的包下标，F4_NO_OPEN 表示无 */
    uint32_t open_epoch;
    uint32_t count;
    uint32_t layout;
    int32_t  fx, fy;      /* XY4 的固定源坐标 */
    uint32_t epoch;
    /* 打开包共享状态的寄存器缓存，f4_open 时写入，热循环不再读包流 */
    uint32_t cur_w0, cur_key, cur_wh;
    /* 上一轮 build 的统计，f4_reset 清零 */
    uint32_t packets4, packets8, inst4, inst8, split_limit, to_xyuv8;
} f4_builder;

/* 错误身份：包零基索引与包内零基索引 */
typedef struct {
    uint32_t pkg, inst;
} f4_id;

static inline uint32_t f4_pack_pair(int32_t a, int32_t b)
{
    return ((uint32_t)(uint16_t)a) | (((uint32_t)(uint16_t)b) << 16);
}

/* 绑定输出缓冲；不分配内存。 */
void f4_init(f4_builder *b, gfx_cmd *stream, uint32_t cap,
             uint8_t *arena, uint32_t arena_bytes);

/* 清输出与统计，epoch 归零；重复用于同一输入的重复构造计时。 */
void f4_reset(f4_builder *b);

/* 把 v1 直通记录流转成 v1-I 包流与 arena。返回 GFX_OK 或 GFX_BUILD_BAD_ARG。 */
int f4_build(f4_builder *b, const gfx_cmd *in, uint32_t n);

/* 新 API 的单实例入口原型：共享状态与当前打开包一致就追加，
   否则开新包。坐标必须可无损编码为 int16。 */
int f4_append_inst(f4_builder *b, uint32_t w0_nonop, uint32_t key, uint32_t wh,
                   int32_t dx, int32_t dy, int32_t sx, int32_t sy);

/* 逐项坐标（生产 gfx_instances_append 的 pos 数组同型）。 */
typedef struct {
    int32_t dx, dy, sx, sy;
} f4_pos;

/* 生产 API 形态的批入口原型：调用方按共享状态与 layout 分好段，
   共享检查段级一次，循环内只做坐标校验与存储。XY4 段要求逐项源坐标
   等于 fixed，违反返回 GFX_BUILD_BAD_ARG 且不产出半包。 */
int f4_append_batch(f4_builder *b, uint32_t w0_nonop, uint32_t key, uint32_t wh,
                    uint32_t layout, int32_t fixed_sx, int32_t fixed_sy,
                    const f4_pos *pos, uint32_t n);

/* 批入口的外部封包：一帧最后一段结束后由调用方触发回填 instance_count。 */
void f4_seal_pub(f4_builder *b);

/* 独立展开器：不引用构造器的任何内部状态。展开失败返回负数；
   正常返回写出的标量命令数，ids 非空时回填每条的 (pkg,inst) 身份。 */
long f4_expand(const gfx_cmd *stream, uint32_t n_stream,
               const uint8_t *arena, uint32_t arena_bytes,
               gfx_cmd *out, uint32_t out_cap, f4_id *ids);

#endif /* F4_INST_CORE_H */
