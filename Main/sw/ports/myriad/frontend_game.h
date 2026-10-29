#ifndef FRONTEND_GAME_H
#define FRONTEND_GAME_H
#include "game/stg_draw.h"
int frontend_game_reset(void);
int frontend_game_compose(const stg_game *g,gfx_list *l,const stg_draw_cfg *cfg,
                           const stg_hud_live *live,stg_frame_stats *stats);
int frontend_game_compose_selected(const stg_game *g,gfx_list *l,int background_mode,
                                    const stg_draw_cfg *cfg,const stg_hud_live *live,
                                    stg_frame_stats *stats);
#endif
