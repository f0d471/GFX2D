/******************************************************************************
* @file gfx_engine.h: 2D 渲染引擎的最小驱动（引擎 P1 B6）
*
* @brief  引擎寄存器在 APB 第 0 页（CPU 地址 0xF810_0000 起），偏移与位号由
*         tools/check/regs_gen.py 生成到同目录的 gfx_regs.h（D51/D55 的单一真源），
*         本层不做第二份表。语义的权威是 Main/doc/赛题二总体设计.md 3.4 节与
*         spec 第 02 章；本文件只封装提交协议。
*
*         接口头文件放 bsp/include、实现放 bsp/ti60/gfx_engine.c：MMIO 地址只在
*         板级层出现（与 board.c 里 display 寄存器的处理同一条规矩），应用经这里
*         的接口用引擎。SW9 的硬件后端、场起始中断与软硬件切换叠在这一层之上，
*         不改本接口（中断驱动的帧节拍属 SW9，最小驱动是纯轮询）。
*
*         提交协议（设计文档 3.4 节与 B5 定案 D53/D54 的约束）：
*           - 非空命令表的 LIST_BASE 必须按 32 字节对齐，未对齐报 FETCH（索引 0、
*             TAG 0）；gfx_engine_submit() 在入口就拦下，应用分配/复制命令表时
*             要自己保证对齐，不能依赖 C 结构体的默认对齐。
*           - DOORBELL 只在 EN=1 且空闲时被受理；忙/排空中写它回 PSLVERR。
*           - DONE/ERR 置位即引擎已静止，等完成不必再查 BUSY，计数器可以立刻读
*             （D20：完成后读、按帧做差在软件里做）。
*           - CPU 读回引擎写过的共享区之前必须失效（E3 规则），失效时机在
*             gfx_engine_wait() 返回之后、由调用方经 board_dcache_invalidate_*
*             完成——失效不进驱动 API，顺序由使用方的判据固定。
******************************************************************************/
#ifndef GFX2D_GFX_ENGINE_H
#define GFX2D_GFX_ENGINE_H

#include <stdint.h>

#include "gfx_regs.h"

/* --- 驱动层错误码（负值；引擎自己的错误码 GFX_ERR_* 经 wait 的出参给出） --- */
#define GFX_ENG_OK          0
#define GFX_ENG_E_ABSENT   (-1)     /* 比特流里没有引擎（ID 对不上） */
#define GFX_ENG_E_ALIGN    (-2)     /* LIST_BASE 未按 32 字节对齐（D53） */
#define GFX_ENG_E_REJECT   (-3)     /* 访问被拒（PSLVERR，如忙时写 DOORBELL） */
#define GFX_ENG_E_TIMEOUT  (-4)     /* 等完成超过轮询上界 */
#define GFX_ENG_E_ARG      (-5)     /* 参数越界（表面号、计数器个数） */
#define GFX_ENG_E_CAPS     (-6)     /* 当前比特流未提供所需能力 */

/* 性能计数器个数（PERF0 + 4*i，i = 0..GFX_PERF_COUNT-1，gfx_regs.h 的真源表） */
#define GFX_ENG_PERF_N     GFX_PERF_COUNT

/* 比特流里有没有引擎：读 ID 与 GFX_ID_VALUE 比对。没有时本层其余接口
   直接返回 GFX_ENG_E_ABSENT，调用方不必先问再用（与 board_present 对
   扫描输出的退回是同一手法）。 */
int        gfx_engine_present(void);

/* 读 CAPS（G1：判 TEX 能力位用；位定义见 gfx_regs.h 的 GFX_CAPS_*_M）。 */
int        gfx_engine_caps_read(uint32_t *caps);

/* 软复位并等排空结束：写 SOFT_RST（写 1 自清），等 DRAINING/BUSY 落下。
   表面表 valid 全被清，重新提交前要重新登记；EN 一起置回 1（DOORBELL 要它）。 */
int        gfx_engine_soft_reset(void);

/* 登记表面：写 SURF[slot] 的三个字，valid 位最后置 1（设计文档 3.4 节）。
   slot 0..15；stride_log2 2..31；format 见 gfx_abi.h 的 GFX_FMT_*（v1 的目的
   只能是 XRGB8888）。space 0 DDR、1 TEX（v1-TEX profile 的 BLIT 源，G1），
   2/3 会被命令层按 BAD_SPACE 拒绝。 */
int        gfx_engine_surf_set_space(int slot, uint32_t base, uint32_t width,
                                     uint32_t height, uint32_t stride_log2,
                                     uint32_t space, uint32_t format);

/* 旧入口：space 恒为 DDR 的包装（保留给既有调用）。 */
int        gfx_engine_surf_set(int slot, uint32_t base, uint32_t width,
                               uint32_t height, uint32_t stride_log2,
                               uint32_t format);

/* TEX 装载窗口（G1，0xF810_4000~0xF810_7FFF）：把 [offset, offset+bytes) 写成
   src 指到的内容 / 读回进 dst。bytes 是 4 的倍数且不越 16 KiB；要求引擎空闲
   （忙或排空中返回 GFX_ENG_E_REJECT），驱动纪律见设计文档 3.4 节 2026-10-07
   定案段——硬件不加互锁，装载/读回/重登记由调用方保证不与命令表并发。
   src/dst 是 CPU 视角地址；装载前调用方对 src 失效缓存（E3 规则）。 */
int        gfx_engine_tex_write(const void *src, uint32_t offset, uint32_t bytes);
int        gfx_engine_tex_read(void *dst, uint32_t offset, uint32_t bytes);

/* 提交一张命令表：写 LIST_BASE/LIST_COUNT 并敲 DOORBELL。list_base 必须按
   32 字节对齐（D53）；list_count 是记录数（含 END，硬件按 COUNT 与 END 先到者
   结束）。返回 GFX_ENG_OK 只表示提交被受理，执行结果经 gfx_engine_wait() 读。 */
int        gfx_engine_submit(uint32_t list_base, uint32_t list_count);

/* 提交 ABI v2 表：表按 32 KiB 对齐，条带数 1..30、log2 3/4/5。
   背景/目的槽与参数写入对应寄存器；高度必须匹配当前 RTL 构型。
   首错或桶溢出的构造表由调用方拒绝，驱动不扫描 DDR 命令表。 */
int gfx_engine_submit_strips(uint32_t list_base, uint32_t lstrips,
                              uint32_t strip_log2, uint32_t bg_id, uint32_t dst_id);

/* 等当前提交结束（DONE/ERR 置位且引擎回空闲，D54），读错误三元组。
   出参可以为 NULL（不关心那一项）。返回值是驱动层状态；引擎报错时返回
   GFX_ENG_OK 且 *err_code 非 0，判别要看出参。 */
int        gfx_engine_wait(uint32_t *err_code, uint32_t *err_index, uint32_t *err_tag);

/* 读性能计数器到 values，n 不超过 GFX_ENG_PERF_N。计数从上次 CNT_CLR 起
   累计，引擎空闲时不动；按帧做差在调用方做（D20）。 */
int        gfx_engine_perf_read(uint32_t *values, uint32_t n);

int gfx_engine_error_instance_read(uint32_t *instance);

/* 计数器清零（CTRL.CNT_CLR，写 1 自清）。 */
int        gfx_engine_perf_clear(void);

#endif /* GFX2D_GFX_ENGINE_H */
