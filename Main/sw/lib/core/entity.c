/******************************************************************************
* @file entity.c: E8 的被测对象（纯算法，板上与主机侧同一份）
*
* @brief  两种布局的一帧更新、补入与命令记录打包，外加计时外的摘要。
*         口径与来由见 core/entity.h 的文件头与
*         Engineering/Todo/hw/E8-CPU每弹幕开销与缓存大小.md 2.1、2.2 节。
*
*         摘要放在**独立的编译单元**里（本文件）：编译器看不到更新循环与摘要
*         在同一个 TU 里，不能因为「没人读这些字段」而删掉写入。-flto 会
*         把这条界限抹掉，所以 E8 的构建关闭跨单元 LTO。
******************************************************************************/

#include "core/entity.h"
#include "core/crc32.h"

/* --- 随机序列 ------------------------------------------------------------- */

uint32_t eb_rng(uint32_t *state)
{
    uint32_t s = *state;

    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *state = s;
    return s;
}

/* 速度：-5.0 ~ +5.0 像素/帧，Q12.4。用两次抽取构造一个对称区间，
   不动点乘除，两端也算得一样。 */
static int32_t rng_vel(uint32_t *rng)
{
    return (int32_t)(eb_rng(rng) % (uint32_t)(2u * EB_V_RANGE + 1u))
         - (int32_t)EB_V_RANGE;
}

void eb_seed(eb_ent *e, uint32_t *rng)
{
    e->x      = EB_SPAWN_X + (int32_t)(eb_rng(rng) % (uint32_t)(EB_SCREEN_W * EB_ONE / 2));
    e->y      = EB_SPAWN_Y + (int32_t)(eb_rng(rng) % (uint32_t)(EB_SCREEN_H * EB_ONE / 2));
    e->vx     = rng_vel(rng);
    e->vy     = rng_vel(rng);
    e->sprite = (uint16_t)(eb_rng(rng) & 0x3Fu);
    e->flags  = 1u;
}

void eb_seed_soa(uint32_t i, int32_t *x, int32_t *y, int32_t *vx, int32_t *vy,
                 uint16_t *sprite, uint16_t *flags, uint32_t *rng)
{
    x[i]      = EB_SPAWN_X + (int32_t)(eb_rng(rng) % (uint32_t)(EB_SCREEN_W * EB_ONE / 2));
    y[i]      = EB_SPAWN_Y + (int32_t)(eb_rng(rng) % (uint32_t)(EB_SCREEN_H * EB_ONE / 2));
    vx[i]     = rng_vel(rng);
    vy[i]     = rng_vel(rng);
    sprite[i] = (uint16_t)(eb_rng(rng) & 0x3Fu);
    flags[i]  = 1u;
}

void eb_soa_split(void *mem, uint32_t n,
                  int32_t **x, int32_t **y, int32_t **vx, int32_t **vy,
                  uint16_t **sprite, uint16_t **flags)
{
    uint8_t *p = (uint8_t *)mem;

    *x      = (int32_t *)(void *)(p);
    *y      = (int32_t *)(void *)(p + (uint32_t)n * 4u);
    *vx     = (int32_t *)(void *)(p + (uint32_t)n * 8u);
    *vy     = (int32_t *)(void *)(p + (uint32_t)n * 12u);
    *sprite = (uint16_t *)(void *)(p + (uint32_t)n * 16u);
    *flags  = (uint16_t *)(void *)(p + (uint32_t)n * 16u + (uint32_t)n * 2u);
}

/* --- 出屏与补入 ----------------------------------------------------------- */

/* x/y 各自超出游戏区两侧 2 倍宽/高时补入。 */
static int is_dead(int32_t x, int32_t y)
{
    return x < EB_DEAD_X_LO || x > EB_DEAD_X_HI ||
           y < EB_DEAD_Y_LO || y > EB_DEAD_Y_HI;
}

/* --- AoS ------------------------------------------------------------------ */

void eb_frame_aos(eb_ent *e, uint32_t n, uint32_t *rng, uint32_t *respawn)
{
    uint32_t i;

    for (i = 0u; i < n; i++) {
        eb_ent *p = &e[i];

        p->x += p->vx;
        p->y += p->vy;
        if (is_dead(p->x, p->y)) {
            eb_seed(p, rng);
            if (respawn != 0)
                (*respawn)++;
        }
    }
}

/* 一条 BLIT 记录的位域；独立测试与 gfx_blit 的记录逐字节对照：
     w0  opcode | eq<<8 | key<<12 | src<<16 | dst<<20 | c<<24
     w1  sx | sy<<16          （按 8x8、12 像素图集取图）
     w2  dx | dy<<16
     w3  w  | h<<16           （无符号 16 位）
     w5  key 颜色字
     w6  必须为零             （BLIT 保留字段）
     w7  零基索引             （gfx_list_append 写的标签） */
static void pack_blit(uint32_t *w, uint32_t idx, int32_t dx, int32_t dy,
                      uint16_t sprite)
{
    w[0] = 2u                                   /* GFX_OP_BLIT */
         | (0u << 8)                            /* GFX_EQ_COPY */
         | (1u << 12)                           /* 色键开 */
         | (EB_SLOT_SRC << 16)
         | (EB_SLOT_DST << 20);
    w[1] = ((uint32_t)(sprite & 7u) * 12u)
         | (((uint32_t)(sprite >> 3) * 12u) << 16);
    w[2] = ((uint32_t)dx & 0xFFFFu) | (((uint32_t)dy & 0xFFFFu) << 16);
    w[3] = (12u) | (12u << 16);                 /* 12x12，S1 的敌弹尺寸 */
    w[4] = 0u;
    w[5] = 0x00123456u;
    w[6] = 0u;
    w[7] = idx;
}

uint32_t eb_frame_aos_cmd(eb_ent *e, uint32_t n, uint32_t *rng, uint32_t *cmd,
                          uint32_t *respawn)
{
    uint32_t i, used = 0u;

    for (i = 0u; i < n; i++) {
        eb_ent *p = &e[i];

        p->x += p->vx;
        p->y += p->vy;
        if (is_dead(p->x, p->y)) {
            eb_seed(p, rng);
            if (respawn != 0)
                (*respawn)++;
        }
        if (p->flags != 0u) {
            pack_blit(&cmd[used * EB_CMD_WORDS], used,
                      p->x >> EB_SHIFT, p->y >> EB_SHIFT, p->sprite);
            used++;
        }
    }
    return used;
}

/* --- SoA ------------------------------------------------------------------ */

void eb_frame_soa(int32_t *x, int32_t *y, int32_t *vx, int32_t *vy,
                  uint16_t *sprite, uint16_t *flags,
                  uint32_t n, uint32_t *rng, uint32_t *respawn)
{
    uint32_t i;

    (void)sprite;
    (void)flags;
    for (i = 0u; i < n; i++) {
        x[i] += vx[i];
        y[i] += vy[i];
        if (is_dead(x[i], y[i])) {
            eb_seed_soa(i, x, y, vx, vy, sprite, flags, rng);
            if (respawn != 0)
                (*respawn)++;
        }
    }
}

uint32_t eb_frame_soa_cmd(int32_t *x, int32_t *y, int32_t *vx, int32_t *vy,
                          uint16_t *sprite, uint16_t *flags,
                          uint32_t n, uint32_t *rng, uint32_t *cmd,
                          uint32_t *respawn)
{
    uint32_t i, used = 0u;

    for (i = 0u; i < n; i++) {
        x[i] += vx[i];
        y[i] += vy[i];
        if (is_dead(x[i], y[i])) {
            eb_seed_soa(i, x, y, vx, vy, sprite, flags, rng);
            if (respawn != 0)
                (*respawn)++;
        }
        if (flags[i] != 0u) {
            pack_blit(&cmd[used * EB_CMD_WORDS], used,
                      x[i] >> EB_SHIFT, y[i] >> EB_SHIFT, sprite[i]);
            used++;
        }
    }
    return used;
}

/* --- 摘要（计时外） ------------------------------------------------------- */

uint32_t eb_digest_aos(const eb_ent *e, uint32_t n)
{
    crc32_ctx ctx;
    uint32_t  i;

    /* 逐个成员喂，与 SoA 的逻辑字段摘要使用相同顺序。 */
    crc32_begin(&ctx);
    for (i = 0u; i < n; i++) {
        crc32_update(&ctx, (const void *)&e[i].x,      4u);
        crc32_update(&ctx, (const void *)&e[i].y,      4u);
        crc32_update(&ctx, (const void *)&e[i].vx,     4u);
        crc32_update(&ctx, (const void *)&e[i].vy,     4u);
        crc32_update(&ctx, (const void *)&e[i].sprite, 2u);
        crc32_update(&ctx, (const void *)&e[i].flags,  2u);
    }
    return crc32_end(&ctx);
}

/* 与 AoS 同一个逻辑顺序：逐实体喂六个字段。 */
uint32_t eb_digest_soa(const int32_t *x, const int32_t *y,
                       const int32_t *vx, const int32_t *vy,
                       const uint16_t *sprite, const uint16_t *flags, uint32_t n)
{
    crc32_ctx ctx;
    uint32_t i;

    crc32_begin(&ctx);
    for (i = 0u; i < n; i++) {
        crc32_update(&ctx, (const void *)&x[i],      4u);
        crc32_update(&ctx, (const void *)&y[i],      4u);
        crc32_update(&ctx, (const void *)&vx[i],     4u);
        crc32_update(&ctx, (const void *)&vy[i],     4u);
        crc32_update(&ctx, (const void *)&sprite[i], 2u);
        crc32_update(&ctx, (const void *)&flags[i],  2u);
    }
    return crc32_end(&ctx);
}

uint32_t eb_digest_cmd(const uint32_t *cmd, uint32_t words)
{
    return crc32_ieee((const void *)cmd, words * 4u);
}
