/******************************************************************************
* @file sh_scene.h: 独立应用「凝时」——时间流动射击(SW21,Superhot-like)的世界层
* @brief 30Hz 确定性逻辑 + 全 3D 平涂发射。自由视角(yaw/pitch),世界时间 = 真实时间
*        × 刻度(玩家动作驱动,下限 0.025);无敌无限补员，预生成连续切面人形与逐面RGB。
*        接线:r4_runtime 以 s_scene==3 分流 tick/emit,不进 r4_world(回放契约不动)。
******************************************************************************/
#ifndef GFX2D_SH_SCENE_H
#define GFX2D_SH_SCENE_H

#include <stdint.h>
#include "gfx/rast_setup.h"
#define SH_PC_RECORD_CAP 2048u
#define SH_CACHE_BYTES 0x100000u
#define SH_WEAPON_CACHE_SLOTS 16u
void sh_world_cache_bind(void *memory,uint32_t bytes);

void     sh_world_init(uint32_t seed);
void     sh_world_tick(uint32_t keys, uint32_t *fail);
uint32_t sh_world_advance(uint32_t keys,uint32_t milliseconds,uint32_t *fail);
void sh_hud_read(uint32_t state[4]);
void     sh_world_look(int32_t dx,int32_t dy);
/* PC software overlay, after the world raster and before presentation. */
void     sh_world_overlay(uint32_t *pixels,unsigned width,unsigned height,unsigned stride);
/* 发射当帧全部几何(房间/家具/敌人/弹/碎块)。返回记录数;超容量返回 UINT32_MAX。 */
uint32_t sh_world_emit(const fx16_mat4 *projection, const rast_viewport *vp,
                       rast_record *out, uint32_t cap);
uint32_t sh_score(void);
uint32_t sh_shield(void);
uint32_t sh_ticks(void);
void sh_profile_clock(uint32_t (*clock_fn)(void));
void sh_profile_read(uint32_t out[12]);
/* Index separating world records from the independently depth-tested viewmodel. */
uint32_t sh_world_weapon_start(void);

#endif /* GFX2D_SH_SCENE_H */
