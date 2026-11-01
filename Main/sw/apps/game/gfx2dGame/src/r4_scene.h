/******************************************************************************
* @file r4_scene.h: 模式 4 的确定性世界逻辑与几何发射（只依赖 lib，可宿主单测）
******************************************************************************/
#ifndef GFX2D_R4_SCENE_H
#define GFX2D_R4_SCENE_H

#include <stdint.h>

#include "gfx/gfx.h"
#include "gfx/rast_setup.h"

#define R4_GATE_SLOTS   4
#define R4_GATE_AHEAD   3           /* 同屏最多渲染的门框数 */

typedef struct {
    uint32_t dist;                  /* 门框所在行程（Q16.16 米，>=0） */
    uint32_t x;                     /* 门框横向位置（世界 x，Q16.16） */
    uint8_t  passed;                /* 相机已穿过 */
    uint8_t  fired;                 /* 已从这门框放过敌弹 */
} r4_gate;

typedef struct {
    int32_t  x, y, z;
    uint8_t  alive;
} r4_bullet;

/* Scene2 reuses the inactive bullet-velocity storage; other scene layouts and
 * replay state bytes remain unchanged. Camera poses advance on metre ticks. */
typedef struct {
    int32_t vx,vy;
    int32_t camera_x,camera_y,next_camera_x,next_camera_y;
} r4_flight_state;
_Static_assert(sizeof(r4_flight_state)<=2u*R4_EBULLET_MAX*sizeof(int32_t),"flight state storage");

typedef struct {
    uint32_t d;                     /* 相机行程（世界 z = -d） */
    int32_t  cam_x;                 /* 自机世界 x；相机小幅周期平移另由视图生成 */
    int32_t  strafe;                /* 本 tick 的横移方向（-1/0/1，给倾斜用） */
    int32_t  ship_y;
    int32_t  pitch;
    r4_bullet enemy;
    int32_t bank;
    union {
        struct {int32_t e_vx[R4_EBULLET_MAX], e_vy[R4_EBULLET_MAX];};
        r4_flight_state flight;
    };
    uint32_t ticks;
    uint32_t score, shield;
    uint32_t hiscore, graze, bombs, invulnerable, respawn;
    uint32_t rings_passed;          /* 场景2：干净穿环数（进洞判定） */
    uint32_t combo;                 /* 场景2：连续穿环连击（决定 boost 档位） */
    uint32_t crash_ticks;           /* 场景2：撞环/撞山后的抖屏+红闪倒计时 */
    uint32_t manual, last_keys, enemy_hp, enemy_hit;
    uint32_t ladder;
    uint32_t scene;                 /* 0=走廊 1=茶杯 2=山脊飞行 */
    r4_bullet blast;
    uint32_t blast_ticks;
    uint32_t pcooldown;             /* 自机弹冷却（tick） */
    r4_gate   gate[R4_GATE_SLOTS];  /* 环形：最近的门框窗口 */
    uint32_t gate_head;             /* 最老门框下标 */
    uint32_t gate_seq;              /* 已生成门框总数 */
    r4_bullet ebul[R4_EBULLET_MAX];
    r4_bullet pbul[R4_PBULLET_MAX];
    uint32_t rng;                   /* xs32 状态 */
} r4_world;

/* 固定 30Hz tick。返回 0 成功（失败只可能是内部不变量，计进 fail 链）。 */
void r4_world_init(r4_world *w, uint32_t seed);
void r4_world_tick(r4_world *w, uint32_t keys, uint32_t *fail);

/* 门框横向位置序列（-1m/0/+1m 循环，种子决定起始），发射与逻辑共用。 */
uint32_t r4_gate_x_of(uint32_t seq, uint32_t rng0);

/* 场景 2 的门环序列是 ±1.4m/0（与走廊不同）；mode_enter 在 world_init 之后
   调用它把初始四个门环挪到山脊序列（后续 recycle 在 tick 内自持）。 */
void r4_scene2_fix_gates(r4_world *w);

/* 场景 2 当前地貌分区（0..3，随门环编排推进）；运行时背景换装和 HUD 用。 */
uint32_t r4_scene2_zone(const r4_world *w);

/* Flight-only, fixed-size geometry acceleration. The reference record stream
 * remains authoritative; cache misses/unsupported capacities use that path. */
typedef struct {uint32_t cold,late,columns,hits,fallback;} r4_flight_cache_stats;
void r4_flight_cache_reset(void);
int r4_flight_prewarm(const r4_world *world,const fx16_mat4 *projection,const rast_viewport *vp);
r4_flight_cache_stats r4_flight_cache_status(void);
#ifdef R4_FLIGHT_TEST
uint32_t r4_flight_reference(const r4_world *,const fx16_mat4 *,const rast_viewport *,rast_record *,uint32_t);
int32_t r4_flight_ground_test(int32_t,uint32_t,int32_t);
int32_t r4_flight_anchor_test(uint32_t,int32_t,int32_t,int32_t);
int32_t r4_flight_grid_test(uint32_t,unsigned);
uint32_t r4_flight_terrain_test(const r4_world *,const fx16_mat4 *,const rast_viewport *,rast_record *,uint32_t);
#endif

/* 分区名与提示语（HUD 左上/左下排版用，与 r4_scene.c 色板同序）。 */
extern const char *const ridge_zone_names[4];
extern const char *const ridge_zone_tags[4];

/* 把当前世界发射成 setup 记录（vp = 子视口，走廊 480x480）。
   ladder: 1=L1 平涂 2=L2 TEX。返回记录数；容量不足返回 UINT32_MAX，不呈现截断物体。
   counts 累计 setup 统计（可为 NULL）。 */
uint32_t r4_world_emit(const r4_world *w, const fx16_mat4 *projection,
                       const rast_viewport *vp, uint32_t ladder,
                       rast_record *out, uint32_t cap, rast_setup_stats *counts);
/* 预热120种相机/墙相位；基础投影/视口/阶梯/相位相同才复用。 */
int r4_world_prewarm(const fx16_mat4 *projection,const rast_viewport *vp,
                     uint32_t ladder,rast_record *scratch,uint32_t cap);

#endif /* GFX2D_R4_SCENE_H */
