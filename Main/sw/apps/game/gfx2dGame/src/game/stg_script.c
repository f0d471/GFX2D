/******************************************************************************
* @file stg_script.c: 游戏判据入口的按键脚本
*
* @brief  口径见 stg_script.h。
******************************************************************************/

#include "stg_script.h"

#include "stg_game.h"

/* 9 种方向：不动、上、下、左、右、左上、右上、左下、右下 */
static const uint32_t dirs[9] = {
    0u, STG_KEY_UP, STG_KEY_DOWN, STG_KEY_LEFT, STG_KEY_RIGHT,
    STG_KEY_UP | STG_KEY_LEFT, STG_KEY_UP | STG_KEY_RIGHT,
    STG_KEY_DOWN | STG_KEY_LEFT, STG_KEY_DOWN | STG_KEY_RIGHT,
};

void stg_script_init(stg_script *s)
{
    xs32_seed(&s->rng, STG_SCRIPT_SEED);
    s->keys = 0u;
}

uint32_t stg_script_keys(stg_script *s, uint32_t frame)
{
    if (frame % STG_SCRIPT_PERIOD == 0u) {
        const uint32_t r = xs32_next(&s->rng);

        s->keys = dirs[r % 9u] | ((((r >> 16) & 3u) == 0u) ? STG_KEY_SLOW : 0u);
    }
    return s->keys | ((frame == STG_SCRIPT_BOMB_AT) ? STG_KEY_BOMB : 0u);
}
