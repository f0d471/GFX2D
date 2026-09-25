/******************************************************************************
* @file stg_pool.c: 实体池
*
* @brief  接口与口径见 stg_pool.h 与 SW7 2.2 节。实体的字段只在本文件里读写。
*         全部是整数运算，没有浮点、没有 malloc、没有文件级的可变状态。
******************************************************************************/

#include "stg_pool.h"

void stg_pool_init(stg_pool *p, stg_ent *storage, uint32_t cap)
{
    p->ent      = storage;
    p->cap      = (storage == 0) ? 0u : cap;
    p->n        = 0u;
    p->overflow = 0u;
}

void stg_pools_init(stg_pools *s)
{
    stg_pool_init(&s->ebullet,  s->ebullet_mem,  STG_CAP_EBULLET);
    stg_pool_init(&s->pbullet,  s->pbullet_mem,  STG_CAP_PBULLET);
    stg_pool_init(&s->enemy,    s->enemy_mem,    STG_CAP_ENEMY);
    stg_pool_init(&s->particle, s->particle_mem, STG_CAP_PARTICLE);
}

void stg_bounds_field(stg_bounds *b)
{
    b->x_lo = -STG_MARGIN_PX * FX_ONE;
    b->x_hi = (STG_FIELD_W + STG_MARGIN_PX) * FX_ONE;
    b->y_lo = -STG_MARGIN_PX * FX_ONE;
    b->y_hi = (STG_FIELD_H + STG_MARGIN_PX) * FX_ONE;
}

int stg_pool_add(stg_pool *p, int32_t x, int32_t y, int32_t vx, int32_t vy,
                 uint16_t sprite, uint16_t flags)
{
    stg_ent *e;

    if (p->n >= p->cap) {
        p->overflow++;
        return -1;
    }
    e = &p->ent[p->n];
    e->x      = x;
    e->y      = y;
    e->vx     = vx;
    e->vy     = vy;
    e->sprite = sprite;
    e->flags  = (uint16_t)(flags & (uint16_t)~STG_F_REMOVE);
    p->n++;
    return 0;
}

#ifndef INJECT_STG_POOL_SWAP

/* 读指针 r 从 0 走到 n，写指针 w 只在实体存活时前进。
   r == w 时那一次写回是自己写给自己，不分支省掉它：存活是常态，多一个比较不划算。

   字段先全部读进局部变量、再逐个写回，不写成 `stg_ent e = ent[r]; ... ent[w] = e;`：
   后一种写法 gcc 13.4（-O2，板上构建）会把 e 在栈上存一份，每个实体多 5 次存储，
   这个循环正是 N_max 要量的那一段。先读后写也让 r == w 时读写同一个实体不出错。 */
void stg_pool_step(stg_pool *p, const stg_bounds *b, stg_step_stats *st)
{
    stg_ent  *ent = p->ent;
    uint32_t  n = p->n, r, w = 0u;
    uint32_t  oob = 0u, marked = 0u;
    int32_t   x_lo = b->x_lo, x_hi = b->x_hi, y_lo = b->y_lo, y_hi = b->y_hi;
#ifdef INJECT_STG_POOL_STALL
    int       stall = 0;
#endif

    for (r = 0u; r < n; r++) {
        const stg_ent *s = &ent[r];
        uint16_t       flags = s->flags;
        uint16_t       sprite = s->sprite;
        int32_t        vx = s->vx, vy = s->vy;
        int32_t        x = s->x + vx, y = s->y + vy;
        stg_ent       *d;

        if ((flags & STG_F_REMOVE) != 0u) {
            marked++;
#ifdef INJECT_STG_POOL_STALL
            stall = 1;
#endif
            continue;
        }
        if (x < x_lo || x >= x_hi || y < y_lo || y >= y_hi) {
            oob++;
#ifdef INJECT_STG_POOL_STALL
            stall = 1;
#endif
            continue;
        }
        d = &ent[w];
        d->x      = x;
        d->y      = y;
        d->vx     = vx;
        d->vy     = vy;
        d->sprite = sprite;
        d->flags  = flags;
#ifdef INJECT_STG_POOL_STALL
        /* 注错：移除之后的第一个存活实体写进去了，写指针却停一步，下一个会把它盖掉 */
        if (stall) {
            stall = 0;
            continue;
        }
#endif
        w++;
    }

    p->n = w;
    if (st != 0) {
        st->oob    = oob;
        st->marked = marked;
    }
}

#else /* INJECT_STG_POOL_SWAP */

/* 注错：常见的「与末尾交换」移除。计数都对，但存活实体的相对顺序被打乱。
   换过来的末尾实体还没更新过，所以原地不前进、下一轮处理它。 */
void stg_pool_step(stg_pool *p, const stg_bounds *b, stg_step_stats *st)
{
    stg_ent  *ent = p->ent;
    uint32_t  n = p->n, i = 0u;
    uint32_t  oob = 0u, marked = 0u;

    while (i < n) {
        stg_ent e = ent[i];

        if ((e.flags & STG_F_REMOVE) != 0u) {
            marked++;
            ent[i] = ent[n - 1u];
            n--;
            continue;
        }
        e.x += e.vx;
        e.y += e.vy;
        if (e.x < b->x_lo || e.x >= b->x_hi || e.y < b->y_lo || e.y >= b->y_hi) {
            oob++;
            ent[i] = ent[n - 1u];
            n--;
            continue;
        }
        ent[i] = e;
        i++;
    }

    p->n = n;
    if (st != 0) {
        st->oob    = oob;
        st->marked = marked;
    }
}

#endif /* INJECT_STG_POOL_SWAP */

void stg_pool_mark(stg_pool *p, uint32_t i)
{
    if (i < p->n)
        p->ent[i].flags = (uint16_t)(p->ent[i].flags | STG_F_REMOVE);
}

uint32_t stg_pool_count(const stg_pool *p)
{
    return p->n;
}

uint32_t stg_pool_overflow(const stg_pool *p)
{
    return p->overflow;
}

int stg_pool_get(const stg_pool *p, uint32_t i, stg_ent *out)
{
    if (i >= p->n)
        return -1;
    *out = p->ent[i];
    return 0;
}
