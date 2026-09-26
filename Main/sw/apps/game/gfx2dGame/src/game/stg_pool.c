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
    p->kinds    = 0u;
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
    p->kinds |= 1u << (flags & STG_F_KIND_MASK);
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
    uint32_t  oob = 0u, marked = 0u, kinds = 0u;
    int32_t   x_lo = b->x_lo, x_hi = b->x_hi, y_lo = b->y_lo, y_hi = b->y_hi;
#ifdef INJECT_STG_POOL_STALL
    int       stall = 0;
#endif

    for (r = 0u; r < n; r++) {
        const stg_ent *s = &ent[r];
        uint16_t       flags = s->flags;
        uint16_t       sprite = s->sprite;
        int32_t        vx = s->vx, vy = s->vy;
#ifdef INJECT_STG_LOOP_DIV
        /* 注错：更新循环里的一次除法，objdump 那条判据必须看见它 */
        int32_t        x = s->x + vx / (int32_t)(sprite | 1u), y = s->y + vy;
#else
        int32_t        x = s->x + vx, y = s->y + vy;
#endif
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
        kinds |= 1u << (flags & STG_F_KIND_MASK);
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
    p->kinds = kinds;
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
    p->kinds = 0xFFFFu;
    if (st != 0) {
        st->oob    = oob;
        st->marked = marked;
    }
}

#endif /* INJECT_STG_POOL_SWAP */

/* 粗筛用一次无符号比较代替两次有符号比较：|d| < T 等价于 0 <= d + T - 1 < 2T - 1。
   d + T - 1 不溢出的前提是 |d| < 2^30，调用方保证坐标差在 ±2^18 以内。

   平方与比较都在 int32 里：通过粗筛之后 |dx|、|dy| < T <= 64 像素 = 2^14，d² < 2^29。 */
void stg_pool_collide(stg_pool *p, const int32_t *r_kind, const stg_probe *pr,
                      stg_hit_stats *st)
{
    stg_ent       *ent = p->ent;
    const uint32_t n = p->n;
    const int32_t  px = pr->px, py = pr->py;
    const int32_t  r_hit = pr->r_hit, r_graze = pr->r_graze;
    const int      hit_en = pr->hit_enabled;
    uint32_t       i, hits = 0u, grazes = 0u;

    for (i = 0u; i < n; i++) {
        stg_ent  *e = &ent[i];
        uint32_t  flags = e->flags;
        int32_t   rb, t, dx, dy, d2, rh;

        if ((flags & STG_F_REMOVE) != 0u)
            continue;
        rb = r_kind[flags & STG_F_KIND_MASK];
        t  = r_graze + rb;
        dx = e->x - px;
        dy = e->y - py;
#ifndef INJECT_COLLIDE_NO_CULL
        if ((uint32_t)(dx + t - 1) >= (uint32_t)(2 * t - 1)
            || (uint32_t)(dy + t - 1) >= (uint32_t)(2 * t - 1))
            continue;
#endif
        d2 = dx * dx + dy * dy;
        rh = r_hit + rb;
#ifdef INJECT_COLLIDE_LE
        if (hit_en && d2 <= rh * rh) {      /* 注错：恰好相切也算命中 */
#else
        if (hit_en && d2 < rh * rh) {
#endif
            hits++;
            e->flags = (uint16_t)(flags | STG_F_REMOVE);
            continue;
        }
        if ((flags & STG_F_GRAZED) == 0u && d2 < t * t) {
            grazes++;
            e->flags = (uint16_t)(flags | STG_F_GRAZED);
        }
    }

    st->hits   = hits;
    st->grazes = grazes;
}

/* 相交判断也用无符号比较：dx 落在 [x0 - w + 1, x1] 里当且仅当
   (uint32)(dx - (x0 - w + 1)) <= (uint32)(x1 - (x0 - w + 1))。前提是裁剪框非空、坐标不越出 ±2^30。
   w/2 写成 w >> 1（w 非负时相同），循环外算一次。 */
uint32_t stg_pool_draw(const stg_pool *p, uint32_t kind, const stg_draw_geom *g,
                       const gfx_rect *clip, uint32_t *cursor, gfx_blit_pos *out, uint32_t cap)
{
    const stg_ent  *ent = p->ent;
    const uint32_t  n = p->n;
    const int32_t   w = g->w, h = g->h;
    const int32_t   hw = w >> 1, hh = h >> 1;
    const uint32_t  cols_log2 = g->cols_log2;
    const uint32_t  col_mask = (1u << cols_log2) - 1u;
    const int32_t   x_lo = clip->x0 - w + 1, y_lo = clip->y0 - h + 1;
    const uint32_t  x_span = (uint32_t)(clip->x1 - x_lo), y_span = (uint32_t)(clip->y1 - y_lo);
    const uint32_t  want = kind & STG_F_KIND_MASK;
    uint32_t        i = *cursor, k = 0u;

    /* 尺寸为 0 的弹种（弹种表里未用的项）没有像素，什么也不发；否则上面的区间会退化成错的 */
    if (w <= 0 || h <= 0) {
        *cursor = n;
        return 0u;
    }

    for (; i < n && k < cap; i++) {
        const stg_ent *e = &ent[i];
        uint32_t       s;
        int32_t        dx, dy;

        /* 已标记移除的，移除位让它不等于任何弹种号 */
        if (((uint32_t)e->flags & (STG_F_REMOVE | STG_F_KIND_MASK)) != want)
            continue;
        dx = (e->x >> FX_SHIFT) - hw;
        if ((uint32_t)(dx - x_lo) > x_span)
            continue;
        dy = (e->y >> FX_SHIFT) - hh;
        if ((uint32_t)(dy - y_lo) > y_span)
            continue;
        s = e->sprite;
        out[k].dx = dx;
        out[k].dy = dy;
        out[k].sx = (int32_t)(s & col_mask) * w;
        out[k].sy = (int32_t)(s >> cols_log2) * h;
        k++;
    }
    *cursor = i;
    return k;
}

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

uint32_t stg_pool_kinds(const stg_pool *p)
{
    return p->kinds;
}

/* 与 stg_pool_collide 同一套粗筛写法：|d| < T 等价于 0 <= d + T - 1 < 2T - 1。 */
uint32_t stg_pool_hit_targets(stg_pool *p, const int32_t *r_kind, const stg_target *tg,
                              uint32_t n, uint32_t *hits)
{
    stg_ent       *ent = p->ent;
    const uint32_t nb = p->n;
    uint32_t       i, t, got = 0u;

    for (i = 0u; i < nb; i++) {
        stg_ent       *e = &ent[i];
        const uint32_t flags = e->flags;
        const int32_t  rb = r_kind[flags & STG_F_KIND_MASK];
        const int32_t  bx = e->x, by = e->y;

        if ((flags & STG_F_REMOVE) != 0u)
            continue;
        for (t = 0u; t < n; t++) {
            const int32_t tt = rb + tg[t].r;
            int32_t       dx, dy;

            if (tg[t].alive == 0)
                continue;
            dx = bx - tg[t].x;
            dy = by - tg[t].y;
#ifdef INJECT_PHIT_LOOP_DIV
            dx += dy / (tt | 1);        /* 注错：循环里的一次除法，objdump 那条判据必须看见 */
#endif
            if ((uint32_t)(dx + tt - 1) >= (uint32_t)(2 * tt - 1)
                || (uint32_t)(dy + tt - 1) >= (uint32_t)(2 * tt - 1))
                continue;
#ifdef INJECT_PHIT_LE
            if (dx * dx + dy * dy <= tt * tt) {     /* 注错：恰好相切也算命中 */
#else
            if (dx * dx + dy * dy < tt * tt) {
#endif
                hits[t]++;
                got++;
                e->flags = (uint16_t)(flags | STG_F_REMOVE);
                break;
            }
        }
    }
    return got;
}

uint32_t stg_pool_cancel(stg_pool *p, stg_pool *sparks, int32_t svx, int32_t svy,
                         uint16_t life, uint16_t spark_kind)
{
    stg_ent       *ent = p->ent;
    const uint32_t n = p->n;
    uint32_t       i, k = 0u;

    for (i = 0u; i < n; i++) {
        stg_ent       *e = &ent[i];
        const uint32_t flags = e->flags;

        if ((flags & (STG_F_REMOVE | STG_F_NOCANCEL)) != 0u)
            continue;
        e->flags = (uint16_t)(flags | STG_F_REMOVE);
        k++;
        (void)stg_pool_add(sparks, e->x, e->y, svx, svy, life, spark_kind);
    }
    return k;
}

void stg_pool_age(stg_pool *p)
{
    stg_ent       *ent = p->ent;
    const uint32_t n = p->n;
    uint32_t       i;

    for (i = 0u; i < n; i++) {
        stg_ent       *e = &ent[i];
        const uint32_t flags = e->flags;
        uint32_t       life = e->sprite;

        if ((flags & STG_F_REMOVE) != 0u)
            continue;
#ifdef INJECT_AGE_LOOP_DIV
        life = life / ((flags & 7u) | 1u);  /* 注错：循环里的一次除法，objdump 那条判据必须看见 */
#endif
        life = (life != 0u) ? life - 1u : 0u;
        e->sprite = (uint16_t)life;
        if (life == 0u)
            e->flags = (uint16_t)(flags | STG_F_REMOVE);
    }
}

int stg_pool_get(const stg_pool *p, uint32_t i, stg_ent *out)
{
    if (i >= p->n)
        return -1;
    *out = p->ent[i];
    return 0;
}
