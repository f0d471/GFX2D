/******************************************************************************
* @file replay_r4.h: 三维演示模式（R4）录像的格式与逐 tick 取键
*
* @brief  与 2D 的 .g2dr（replay.h）**语义独立**的新格式：R4 的逻辑时钟是
*         30Hz 固定 tick（每 tick = 2 场），帧数、节拍与场景身份都是头部的
*         显式字段，解析器不默认它们。定长、无对齐洞、多字节量显式小端，
*         LP64 / ILP32 / Python 三方对同一字节串解析出同一串按键。
*
*         字节布局（偏移：含义），总长 32 + 4N：
*
*             0  4  魔数 'R','4','R','P'，小端读作 REPLAY_R4_MAGIC
*             4  2  版本号 REPLAY_R4_VERSION
*             6  2  标志位，当前必须是 0
*             8  4  tick 频率，v1 必须是 30
*            12  4  随机种子
*            16  4  场景身份（scene_id：应用层常量，格式层只透传）
*            20  4  阶梯 REPLAY_R4_L1 / REPLAY_R4_L2
*            24  4  素材版本（应用层常量，格式层只透传）
*            28  4  tick 数 N
*            32  4N 每逻辑 tick 一个 32 位按键位图，位分配同 board.h
*
*         解析拒绝：错魔数、错版本、非零保留位、tick_hz != 30、非法阶梯、
*         长度不精确匹配（截断与多余字节都拒绝）。场景身份与素材版本是否
*         匹配是应用判据，本层不解释。旧的 .g2dr 解析器与文件不改任何语义。
******************************************************************************/
#ifndef GFX2D_CORE_REPLAY_R4_H
#define GFX2D_CORE_REPLAY_R4_H

#include <stdint.h>

#define REPLAY_R4_MAGIC         0x50523452u  /* 文件头前四字节是 'R','4','R','P' */
#define REPLAY_R4_VERSION       1u
#define REPLAY_R4_HEADER_BYTES  32u
#define REPLAY_R4_RECORD_BYTES  4u
#define REPLAY_R4_TICK_HZ       30u

#define REPLAY_R4_L1            1u           /* 平涂 */
#define REPLAY_R4_L2            2u           /* TEX 纹理 */

/* 解析失败的原因。都不是按键内容的问题，是证据载体坏了。 */
#define REPLAY_R4_E_MAGIC       (-1)
#define REPLAY_R4_E_VERSION     (-2)
#define REPLAY_R4_E_FLAGS       (-3)
#define REPLAY_R4_E_TICK        (-4)
#define REPLAY_R4_E_LADDER      (-5)
#define REPLAY_R4_E_LEN         (-6)
#define REPLAY_R4_E_RANGE       (-7)

/* 一段 frames 个 tick 的录像占多少字节。 */
uint32_t replay_r4_bytes_for(uint32_t frames);

/* 把 frames 个 tick 的按键一次打包进 buf。成功返回总字节数；
   cap 不够或参数非法时一个字都不写，返回 0（不截断）。 */
uint32_t replay_r4_pack(uint8_t *buf, uint32_t cap, uint32_t seed, uint32_t scene_id,
                        uint32_t ladder, uint32_t asset_version,
                        const uint32_t *keys, uint32_t frames);

/* --- 流式录制 ----------------------------------------------------------- */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t frames;        /* 已成功追加的 tick 数 */
    uint32_t seed, scene_id, ladder, asset_version;
    int      full;
} replay_r4_rec;

void     replay_r4_rec_begin(replay_r4_rec *r, uint8_t *buf, uint32_t cap,
                             uint32_t seed, uint32_t scene_id, uint32_t ladder,
                             uint32_t asset_version);
int      replay_r4_rec_put(replay_r4_rec *r, uint32_t keys);   /* 0 成功，-1 缓冲满 */
uint32_t replay_r4_rec_frames(const replay_r4_rec *r);

/* 收尾：把头部的 tick 数改成实际录到的数。返回总字节数。 */
uint32_t replay_r4_rec_end(replay_r4_rec *r);

/* --- 读取 --------------------------------------------------------------- */
typedef struct {
    const uint8_t *buf;
    uint32_t       len;
    uint32_t       seed, scene_id, ladder, asset_version;
    uint32_t       frames;
} replay_r4_view;

/* 校验并解析。返回 0 成功，负值是 REPLAY_R4_E_*。out 只在成功时被改写。 */
int replay_r4_open(const uint8_t *buf, uint32_t len, replay_r4_view *out);

/* 取第 frame 个 tick 的按键。越界返回非 0，*keys 不被改写。 */
int replay_r4_key_at(const replay_r4_view *v, uint32_t frame, uint32_t *keys);

/* --- 录制文本 ----------------------------------------------------------- */
/* 录制结果就是日志，脚本照这里的行格式解析（与 g2dr 的两行制分开）：

     r4rp 1 <seed> <scene> <ladder> <assets> <ticks>    头一行
     r4key <tick> <8 位十六进制>                         之后每 tick 一行 */
uint32_t replay_r4_text_header(uint32_t seed, uint32_t scene_id, uint32_t ladder,
                               uint32_t asset_version, uint32_t frames,
                               char *buf, uint32_t cap);
uint32_t replay_r4_text_key(uint32_t frame, uint32_t keys, char *buf, uint32_t cap);

#endif /* GFX2D_CORE_REPLAY_R4_H */
