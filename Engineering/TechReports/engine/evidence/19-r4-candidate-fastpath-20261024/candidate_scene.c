/******************************************************************************
* @file r4_scene.c: 模式 4 的确定性世界逻辑与几何发射
*
* @brief  逻辑时钟 = 固定 30Hz tick：位置只由 tick 与显式种子决定，不读墙钟。
*         相机无旋转，视空间 = 世界 - 相机（纯减法，无矩阵乘、无除法）；
*         这是研究 §4.3 优化清单的第一条（共享顶点只变换一次、不做通用矩阵）。
*
*         走廊墙按 R4_SEG_LEN 网格锚定行程，纹理/顶点图案对世界静止（L2 不游动）；
*         墙起点距相机 R4_NEAR_PAD，恒在近面之前，稳定态不被近面拆分。
*         门框、弹、自机全走 3D 通路：遮挡由深度测试给出（评审二次修订定案）。
*
*         发射顺序 = 预算优先级：墙 > 门框 > 自机 > 自机弹 > 敌弹。
*
*         绕序推演（视空间 x 右 y 上、屏幕 y 翻转，area>0 为可见）：
*         侧墙 =（近上、远上、远下、近下）；地板 =（近左、远左、远右、近右）；
*         天花板取地板反绕；面向相机的面 =（左上、右上、右下、左下）。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "board.h"

#include "r4.h"
#include "r4_scene.h"
#include "core/xorshift32.h"

/* 每 tick 的位移（Q16.16）：整除截断是口径的一部分，两边一致即确定。 */
#define R4_CAM_STEP         ((4 * R4_ONE) / R4_TICK_HZ)
#define R4_STRAFE_STEP      ((4 * R4_ONE) / R4_TICK_HZ)
#define R4_EBULLET_STEP     ((6 * R4_ONE) / R4_TICK_HZ)
#define R4_PBULLET_STEP     ((12 * R4_ONE) / R4_TICK_HZ)

#define R4_CAM_X_LIMIT      (R4_HALF_W - R4_ONE / 2)    /* 离墙 0.5m */
#define R4_PCOOLDOWN        15u
#define R4_GATE_LOOKAHEAD   (3u * R4_GATE_SPACING)
#define R4_GATE_FIRE_AHEAD  (8u * R4_ONE)
#define R4_DRAW_FAR         (32u * R4_ONE)              /* 远于它的门框/弹不发射 */
#define R4_BULLET_ARRIVE    (-(int32_t)(R4_NEAR_Z + R4_ONE / 2))

void r4_world_init(r4_world *w, uint32_t seed)
{
    xs32 g;
    uint32_t i;

    for (i = 0u; i < sizeof(*w); i++)
        ((uint8_t *)w)[i] = 0u;
    xs32_seed(&g, seed);
    w->rng = xs32_next(&g) % 3u;
    w->shield = R4_SHIELD_INIT;
    w->ship_y = -R4_ONE;
    w->gate[0].dist = R4_GATE_FIRST;
    w->gate[0].x = r4_gate_x_of(0u, w->rng);
    w->gate_seq = 1u;
    w->enemy.alive = 1u;
    w->enemy.z = -(int32_t)(R4_GATE_FIRST + 2u * R4_ONE);
}

/* 门框横向位置序列：-1m / 0 / +1m 循环，种子决定起始。 */
uint32_t r4_gate_x_of(uint32_t seq, uint32_t rng0)
{
    static const int32_t pos[3] = { -(int32_t)R4_ONE, 0, (int32_t)R4_ONE };

    return (uint32_t)pos[(seq + rng0) % 3u];
}

static void spawn_gate(r4_world *w)
{
    r4_gate *g = &w->gate[w->gate_seq % R4_GATE_SLOTS];

    g->dist = R4_GATE_FIRST + w->gate_seq * R4_GATE_SPACING;
    g->x = r4_gate_x_of(w->gate_seq, w->rng);
    g->passed = 0u;
    g->fired = 0u;
    w->gate_seq++;
    if (w->gate_seq > R4_GATE_SLOTS)
        w->gate_head = (w->gate_head + 1u) % R4_GATE_SLOTS;
}

static void spawn_ebullet(r4_world *w, int32_t x, int32_t z)
{
    uint32_t i;

    for (i = 0u; i < R4_EBULLET_MAX; i++) {
        if (!w->ebul[i].alive) {
            w->ebul[i].alive = 1u;
            w->ebul[i].x = x;
            w->ebul[i].y = w->ship_y;
            w->ebul[i].z = z;
            return;
        }
    }
}

static void spawn_pbullet(r4_world *w)
{
    uint32_t i;

    for (i = 0u; i < R4_PBULLET_MAX; i++) {
        if (!w->pbul[i].alive) {
            w->pbul[i].alive = 1u;
            w->pbul[i].x = w->cam_x;
            w->pbul[i].y = w->ship_y;
            w->pbul[i].z = -(int32_t)w->d - (int32_t)R4_SHIP_Z;
            return;
        }
    }
}

void r4_world_tick(r4_world *w, uint32_t keys, uint32_t *fail)
{
    uint32_t i, j;
    int32_t dir = 0;

    (void)fail;
    w->ticks++;
    if (keys & BOARD_KEY_LEFT)
        dir -= 1;
    if (keys & BOARD_KEY_RIGHT)
        dir += 1;
    w->strafe = dir;
    w->pitch=((keys&BOARD_KEY_UP)!=0)-((keys&BOARD_KEY_DOWN)!=0);
    w->cam_x += dir * R4_STRAFE_STEP;
    if (w->cam_x > R4_CAM_X_LIMIT)
        w->cam_x = R4_CAM_X_LIMIT;
    if (w->cam_x < -(int32_t)R4_CAM_X_LIMIT)
        w->cam_x = -(int32_t)R4_CAM_X_LIMIT;
    w->d += R4_CAM_STEP;
    if (keys & BOARD_KEY_UP) w->ship_y += R4_STRAFE_STEP/2;
    if (keys & BOARD_KEY_DOWN) w->ship_y -= R4_STRAFE_STEP/2;
    if (w->ship_y > R4_ONE) w->ship_y = R4_ONE;
    if (w->ship_y < -R4_ONE*5/4) w->ship_y = -R4_ONE*5/4;

    if (w->pcooldown != 0u)
        w->pcooldown--;
    if (w->pcooldown == 0u) {
        spawn_pbullet(w);
        w->pcooldown = R4_PCOOLDOWN;
    }

    while (R4_GATE_FIRST + w->gate_seq * R4_GATE_SPACING < w->d + R4_GATE_LOOKAHEAD)
        spawn_gate(w);

    if (w->enemy.alive) {
        w->enemy.x = (fx_sin((uint16_t)(w->ticks*512u))*6);
        if ((int32_t)w->d+w->enemy.z > -(int32_t)R4_SHIP_Z) w->enemy.alive=0;
        if ((w->ticks%90u)==0u) spawn_ebullet(w,w->enemy.x,w->enemy.z);
    } else if ((w->ticks%120u)==0u) {
        w->enemy.alive=1u;
        w->enemy.z=-(int32_t)(w->d+20u*R4_ONE);
    }

    for (i = 0u; i < R4_GATE_SLOTS; i++) {
        r4_gate *g = &w->gate[i];

        if (g->dist == 0u)
            continue;
        if (!g->fired && g->dist > w->d && g->dist < w->d + R4_GATE_FIRE_AHEAD) {
            g->fired = 1u;
            spawn_ebullet(w, (int32_t)g->x, -(int32_t)g->dist);
        }
        if (!g->passed && w->d >= g->dist) {
            const int32_t margin = (int32_t)(R4_GATE_OPEN - R4_SHIP_HALF);

            g->passed = 1u;
            if (w->cam_x < (int32_t)g->x-margin ||
                w->cam_x > (int32_t)g->x+margin) {
                if (w->shield != 0u)
                    w->shield--;            /* 撞框 */
            } else {
                w->score += 50u;            /* 过门 */
            }
        }
    }

    for (i = 0u; i < R4_EBULLET_MAX; i++) {
        r4_bullet *b = &w->ebul[i];

        if (!b->alive)
            continue;
        b->z += R4_EBULLET_STEP;
        /* 视 z = b->z + d；到达相机面按世界横向距离判命中/擦弹。 */
        if (b->z + (int32_t)w->d >= R4_BULLET_ARRIVE) {
            int32_t dx = b->x - w->cam_x;
            int32_t dy = b->y - w->ship_y;

            b->alive = 0u;
            if (dx < R4_GRAZE_DIST && dx > -R4_GRAZE_DIST &&
                dy < R4_GRAZE_DIST && dy > -R4_GRAZE_DIST) {
                if (dx < R4_HIT_DIST && dx > -R4_HIT_DIST &&
                    dy < R4_HIT_DIST && dy > -R4_HIT_DIST) {
                    if (w->shield != 0u)
                        w->shield--;
                } else {
                    w->score += 10u;        /* 擦弹 */
                }
            }
        }
    }

    for (i = 0u; i < R4_PBULLET_MAX; i++) {
        r4_bullet *p = &w->pbul[i];

        if (!p->alive)
            continue;
        p->z -= R4_PBULLET_STEP;
        if (p->z + (int32_t)w->d < -(int32_t)R4_DRAW_FAR)
            p->alive = 0u;
        for (j = 0u; j < R4_EBULLET_MAX; j++) {
            r4_bullet *e = &w->ebul[j];
            int32_t dx = p->x - e->x;
            int32_t dz = p->z - e->z;

            if (!e->alive)
                continue;
            if (dx < R4_HIT_DIST && dx > -R4_HIT_DIST &&
                dz < R4_HIT_DIST && dz > -R4_HIT_DIST) {
                p->alive = 0u;
                e->alive = 0u;
                w->score += 100u;
            }
        }
    }
}

/* --- 几何发射 -------------------------------------------------------------- */
static int emit_failed;

static void set_v(rast_view_vertex *v, int32_t x, int32_t y, int32_t z,
                  uint16_t u, uint16_t t, uint32_t color)
{
    v->x = x;
    v->y = y;
    v->z = z;
    v->u = u;
    v->v = t;
    v->color = color;
}

static uint32_t emit_view(const rast_view_vertex v[4], const rast_viewport *vp,
                          uint32_t flags, const fx16_mat4 *projection, rast_record *out,
                          uint32_t cap, rast_setup_stats *delta)
{
    int n = rast_setup_view_vp(v, 4u, projection, R4_NEAR_Z, R4_FAR_Z, vp, flags,
                               out, cap, delta);

    if(n<0)emit_failed=1;
    return (n > 0) ? (uint32_t)n : 0u;
}

/* 面向相机的四边形（billboard / 门框 / 自机）。uo/vo = atlas 区块原点（texel）。 */
static uint32_t emit_facing(int32_t cx, int32_t cy, int32_t z, int32_t hw, int32_t hh,
                            uint32_t color, uint16_t uo, uint16_t vo, uint32_t flags,
                            const rast_viewport *vp, const fx16_mat4 *projection,
                            rast_record *out, uint32_t cap, rast_setup_stats *delta)
{
    rast_view_vertex v[4];
    const uint16_t u1 = (uint16_t)(uo + 31u * 16u);
    const uint16_t v1 = (uint16_t)(vo + 31u * 16u);

    set_v(&v[0], cx - hw, cy + hh, z, uo, vo, color);
    set_v(&v[1], cx + hw, cy + hh, z, u1, vo, color);
    set_v(&v[2], cx + hw, cy - hh, z, u1, v1, color);
    set_v(&v[3], cx - hw, cy - hh, z, uo, v1, color);
    return emit_view(v, vp, flags, projection, out, cap, delta);
}

/* 侧墙（朝内）：左墙=近上、远上、远下、近下；右墙屏幕绕序镜像，取反序。 */
static uint32_t emit_side_wall(int32_t x, int32_t z_near, int32_t z_far, uint32_t right,
                               uint32_t color, uint16_t uo, uint16_t vo, uint32_t flags,
                               const rast_viewport *vp, const fx16_mat4 *projection,
                               rast_record *out, uint32_t cap, rast_setup_stats *delta)
{
    rast_view_vertex v[4];
    const uint16_t v1 = (uint16_t)(vo + 31u * 16u);
    const uint16_t u1 = (uint16_t)(uo + 31u * 16u);

    if (right) {
        set_v(&v[0], x, -(int32_t)R4_HALF_W, z_near, u1, vo, color);
        set_v(&v[1], x, -(int32_t)R4_HALF_W, z_far, u1, v1, color);
        set_v(&v[2], x, (int32_t)R4_HALF_W, z_far, uo, v1, color);
        set_v(&v[3], x, (int32_t)R4_HALF_W, z_near, uo, vo, color);
    } else {
        set_v(&v[0], x, (int32_t)R4_HALF_W, z_near, uo, vo, color);
        set_v(&v[1], x, (int32_t)R4_HALF_W, z_far, uo, v1, color);
        set_v(&v[2], x, -(int32_t)R4_HALF_W, z_far, u1, v1, color);
        set_v(&v[3], x, -(int32_t)R4_HALF_W, z_near, u1, vo, color);
    }
    return emit_view(v, vp, flags, projection, out, cap, delta);
}

/* 地板/天花板：天花板取地板反绕。 */
static uint32_t emit_h_wall(int32_t y, int32_t x_left, int32_t x_right,
                            int32_t z_near, int32_t z_far, uint32_t ceiling,
                            uint32_t color, uint16_t uo, uint16_t vo, uint32_t flags,
                            const rast_viewport *vp, const fx16_mat4 *projection,
                            rast_record *out, uint32_t cap, rast_setup_stats *delta)
{
    rast_view_vertex v[4];
    const uint16_t u1 = (uint16_t)(uo + 31u * 16u);
    const uint16_t v1 = (uint16_t)(vo + 31u * 16u);

    if (ceiling) {
        set_v(&v[0], x_right, y, z_near, uo, vo, color);
        set_v(&v[1], x_right, y, z_far, uo, v1, color);
        set_v(&v[2], x_left, y, z_far, u1, v1, color);
        set_v(&v[3], x_left, y, z_near, u1, vo, color);
    } else {
        set_v(&v[0], x_left, y, z_near, uo, vo, color);
        set_v(&v[1], x_left, y, z_far, uo, v1, color);
        set_v(&v[2], x_right, y, z_far, u1, v1, color);
        set_v(&v[3], x_right, y, z_near, u1, vo, color);
    }
    return emit_view(v, vp, flags, projection, out, cap, delta);
}

uint32_t r4_world_emit(const r4_world *w, const fx16_mat4 *projection,
                       const rast_viewport *vp, uint32_t ladder,
                       rast_record *out, uint32_t cap, rast_setup_stats *counts)
{
    rast_setup_stats delta = { 0, 0, 0, 0 };
    const uint32_t flags_flat = 2u;                 /* VCOLOR_EN */
    const uint32_t flags_opaque_tex = 1u;           /* TEX_EN，无色键 */
    const uint32_t flags_key_tex = 1u | 4u;         /* TEX_EN|KEY_EN */
    const uint32_t flags_wall = (ladder >= 2u) ? flags_opaque_tex : flags_flat;
    uint32_t produced = 0u;
    int32_t z_near, z_far;
    int32_t wl, wr;
    uint32_t i;
    uint32_t nearest = R4_GATE_SLOTS, nearest_dist = UINT32_MAX;
    emit_failed=0;
    if(!out || cap<128u)return UINT32_MAX;

    /* 墙锚定：k0 = ceil((d+NEAR_PAD)/SEG)；墙近缘视 z = -(k0*SEG - d)。 */
    {
        z_near = -(int32_t)(R4_ONE / 2);
        z_far = -(int32_t)(R4_SEG_COUNT * R4_SEG_LEN + R4_SEG_LEN - w->d % R4_SEG_LEN);
    }
    wl = -(int32_t)R4_HALF_W - w->cam_x;
    wr = (int32_t)R4_HALF_W - w->cam_x;

    produced += emit_side_wall(wl, z_near, z_far, 0u, R4_COLOR_WALL, 0u, 0u,
                               flags_wall, vp, projection, out + produced,
                               cap - produced, &delta);
    produced += emit_side_wall(wr, z_near, z_far, 1u, R4_COLOR_WALL, 0u, 0u,
                               flags_wall, vp, projection, out + produced,
                               cap - produced, &delta);
    produced += emit_h_wall(-(int32_t)R4_HALF_W, wl, wr, z_near, z_far, 0u,
                            R4_COLOR_FLOOR, 0u, 0u, flags_wall, vp, projection,
                            out + produced, cap - produced, &delta);
    produced += emit_h_wall((int32_t)R4_HALF_W, wl, wr, z_near, z_far, 1u,
                            R4_COLOR_CEIL, 0u, 0u, flags_wall, vp, projection,
                            out + produced, cap - produced, &delta);

    /* 门框：两柱两梁。 */
    for (i=0;i<R4_GATE_SLOTS;++i) {
        if (w->gate[i].dist>w->d && w->gate[i].dist<nearest_dist) {
            nearest=i;nearest_dist=w->gate[i].dist;
        }
    }
    for (i = 0u; i < R4_GATE_SLOTS; i++) {
        const r4_gate *g = &w->gate[i];
        int32_t gz, gx, o, t;
        uint32_t flags = (ladder >= 2u) ? flags_opaque_tex : flags_flat;
        if(i!=nearest) continue;

        if (g->dist == 0u || g->dist < w->d)
            continue;
        gz = (int32_t)w->d - (int32_t)g->dist;
        if (gz > -(int32_t)(R4_ONE / 2) || gz < -(int32_t)R4_DRAW_FAR)
            continue;
        gx = (int32_t)g->x - w->cam_x;
        o = (int32_t)R4_GATE_OPEN;
        t = (int32_t)R4_GATE_THICK;
        produced += emit_facing(gx - o - t, 0, gz, t, (int32_t)R4_HALF_W,
                                R4_COLOR_GATE, 32u * 16u, 0u, flags, vp,
                                projection, out + produced, cap - produced, &delta);
        produced += emit_facing(gx + o + t, 0, gz, t, (int32_t)R4_HALF_W,
                                R4_COLOR_GATE, 32u * 16u, 0u, flags, vp,
                                projection, out + produced, cap - produced, &delta);
        produced += emit_facing(gx, (int32_t)R4_HALF_W - t / 2, gz, o + t, t,
                                R4_COLOR_GATE, 32u * 16u, 0u, flags, vp,
                                projection, out + produced, cap - produced, &delta);
        produced += emit_facing(gx, -(int32_t)R4_HALF_W + t / 2, gz, o + t, t,
                                R4_COLOR_GATE, 32u * 16u, 0u, flags, vp,
                                projection, out + produced, cap - produced, &delta);
    }

    if (ladder>=2u && w->enemy.alive) {
        produced += emit_facing(w->enemy.x-w->cam_x,0,(int32_t)w->d+w->enemy.z,
            R4_ONE/2,R4_ONE/2,0xffffffffu,0,32u*16u,flags_key_tex,
            vp,projection,out+produced,cap-produced,&delta);
    }

    /* Five actual 3D faces: banking wings, pitched fuselage and an engine flame. */
    {
        const int32_t local[8][3]={{0,23000,-6000},{28000,-11000,7000},
            {0,-5000,-4000},{-28000,-11000,7000},{0,-21000,10000},
            {-5000,-18000,11000},{5000,-18000,11000},{0,-31000,15000}};
        const unsigned faces[5][3]={{0,1,2},{0,2,3},{2,1,4},{2,4,3},{5,6,7}};
        rast_view_vertex verts[8];
        int32_t bank=w->strafe*18000;
        for(unsigned k=0;k<8;++k) {
            int32_t x=fx16_mul(local[k][0],63016)-fx16_mul(local[k][1],bank);
            int32_t y=fx16_mul(local[k][0],bank)+fx16_mul(local[k][1],63016);
            set_v(&verts[k],x,y+w->ship_y,-(int32_t)R4_SHIP_Z+local[k][2]+
                  w->pitch*local[k][1]/3+w->strafe*local[k][0]/4,0,0,0xffffffffu);
        }
        for(unsigned k=0;k<5;++k) {
            rast_view_vertex tri[3];
            uint32_t col=k==4?0xffffa030u:(k&1u)?0xff208868u:0xff60ecc0u;
            for(unsigned j=0;j<3;++j){tri[j]=verts[faces[k][j]];tri[j].color=col;}
            int n=rast_setup_view_vp(tri,3,projection,R4_NEAR_Z,R4_FAR_Z,vp,
                                    flags_flat,out+produced,cap-produced,&delta);
            if(n<0)emit_failed=1;else produced+=(unsigned)n;
        }
    }

    /* 自机弹（纵向小条）。 */
    for (i = 0u; i < R4_PBULLET_MAX; i++) {
        const r4_bullet *b = &w->pbul[i];
        int32_t vz;

        if (!b->alive)
            continue;
        vz = (int32_t)w->d + b->z;
        if (vz > -(int32_t)(R4_ONE / 2) || vz < -(int32_t)R4_DRAW_FAR)
            continue;
        produced += emit_facing(b->x - w->cam_x, b->y,
                                vz, R4_BULLET_HALF / 3, R4_BULLET_HALF,
                                R4_COLOR_PBULLET, 0u, 0u, flags_flat, vp,
                                projection, out + produced, cap - produced, &delta);
    }

    /* 敌弹：L1 平涂方块，L2 色键抠底 billboard。 */
    for (i = 0u; i < R4_EBULLET_MAX; i++) {
        const r4_bullet *b = &w->ebul[i];
        int32_t vz;
        uint32_t flags = (ladder >= 2u) ? flags_key_tex : flags_flat;

        if (!b->alive)
            continue;
        vz = (int32_t)w->d + b->z;
        if (vz > -(int32_t)(R4_ONE / 2) || vz < -(int32_t)R4_DRAW_FAR)
            continue;
        produced += emit_facing(b->x - w->cam_x, b->y, vz, R4_BULLET_HALF,
                                R4_BULLET_HALF, R4_COLOR_EBULLET, 32u * 16u,
                                32u * 16u, flags, vp, projection, out + produced,
                                cap - produced, &delta);
    }

    if (counts) {
        counts->backface += delta.backface;
        counts->sliver += delta.sliver;
        counts->clipped += delta.clipped;
        counts->emitted += delta.emitted;
    }
    return emit_failed ? UINT32_MAX : produced;
}
