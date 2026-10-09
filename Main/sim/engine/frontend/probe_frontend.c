/* 研究探针：真实游戏的命令按绘制阶段计数；不执行像素，不测板上周期。 */
#include "gfx/gfx.h"
#include "core/assetpak.h"
#include "game/stg_game.h"
#include "game/stg_kind.h"
#include "stg_modes.h"
#include "assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int stg_game_compose_observed(const stg_game *, gfx_list *, const stg_draw_cfg *,
                              const stg_hud_live *, stg_frame_stats *);
static unsigned marks[9];
void frontend_mark(gfx_list *list, unsigned stage) { marks[stage] = list->used; }
static unsigned char pak_mem[0x300000];
static unsigned char tables[2][GFX_STRIP_TABLE_BYTES] __attribute__((aligned(32768)));
static stg_game game;
static const unsigned entries[13] = {
    ASSET_BUL16_ENTRY, ASSET_BUL8_ENTRY, ASSET_RICE20_ENTRY, ASSET_FONT8X16_ENTRY,
    ASSET_PSHOT_ENTRY, ASSET_SPARK_ENTRY, ASSET_SHIPS_ENTRY, ASSET_BOSS_ENTRY,
    ASSET_STARS_ENTRY, ASSET_TITLE_ENTRY, ASSET_DIGITS_ENTRY, ASSET_ICONS_ENTRY,
    ASSET_FONT_CYAN_ENTRY
};

static void same(const gfx_list *a, const gfx_list *b, const stg_frame_stats *sa,
                 const stg_frame_stats *sb)
{
    if (a->used != b->used || a->first_error != b->first_error ||
        a->rejected != b->rejected || a->overflow != b->overflow || a->ended != b->ended ||
        memcmp(sa, sb, sizeof *sa) || memcmp(tables[0], tables[1], sizeof tables[0])) {
        fputs("observer changed production output\n", stderr); exit(20);
    }
    if (a->first_error || a->rejected || a->overflow) {
        fputs("production rejected research scene\n", stderr); exit(21);
    }
}

int main(int argc, char **argv)
{
    gfx_surface_table tab;
    gfx_surface dst = {0x2000000u, 640u, 480u, 12u, 0u, 0u};
    gfx_strip_config cfg = {30u, 4u, 0u, 0u};
    assetpak pak;
    assetpak_limits limit = {sizeof pak_mem};
    stg_draw_cfg draw;
    stg_hud_live live = {0};
    const unsigned knobs[] = {0u, 384u, 1024u, 1648u, 1744u, 2400u, 2800u};
    unsigned kind, ki, i, frame, off = 0u;
    FILE *f;
    size_t bytes;
    if (argc != 2) return 2;
    f = fopen(argv[1], "rb"); if (!f) return 3;
    bytes = fread(pak_mem, 1, sizeof pak_mem, f); fclose(f);
    if (assetpak_parse(&pak, pak_mem, (unsigned)bytes, &limit)) return 4;
    gfx_surface_reset(&tab);
    if (gfx_surface_set(&tab, 0, &dst) || gfx_surface_set(&tab, 1, &dst)) return 5;
    for (i = 0; i < 13; i++) {
        assetpak_entry e;
        gfx_surface s;
        if (assetpak_entry_at(&pak, entries[i], &e)) return 6;
        s = (gfx_surface){0x1000000u + e.offset, e.width, e.height, e.stride_log2, 0u, e.format};
        if (i == 0 || i == 1 || i == 5) { s.base = off; s.space = 1; off += e.length; }
        if (off > 16384u || gfx_surface_set(&tab, (int)i + 2, &s)) return 7;
    }
    stg_modes_draw_cfg(&draw, 0);
    puts("kind,knob,frame,active,total_v1,total_v2,bg_v1,bg_v2,actors_v1,actors_v2,pbullet_v1,pbullet_v2,ebullet_v1,ebullet_v2,particle_v1,particle_v2,overlay_v1,overlay_v2,hud_v1,hud_v2");
    for (kind = 0; kind < 2; kind++) for (ki = 0; ki < sizeof knobs / sizeof knobs[0]; ki++) {
        stg_game_init(&game, STG_GAME_SEED);
        stg_game_press_begin(&game, kind == 0 ? STG_KIND_ROUND16 : STG_KIND_SMALL8);
        stg_game_set_knob(&game, knobs[ki]);
        for (i = 0; i < 1200; i++) stg_game_step(&game, 0);
        memset(tables, 0, sizeof tables);
        for (frame = 0; frame < 120; frame++) {
            unsigned counts[2][9], mode;
            stg_frame_stats sa, sb;
            for (mode = 0; mode < 2; mode++) {
                gfx_list a, b;
                if (mode == 0) {
                    gfx_list_begin(&a, (gfx_cmd *)(void *)tables[0], 32768u, &tab);
                    gfx_list_begin(&b, (gfx_cmd *)(void *)tables[1], 32768u, &tab);
                } else if (gfx_list_begin_strips(&a, tables[0], sizeof tables[0], &tab, &cfg, NULL) ||
                           gfx_list_begin_strips(&b, tables[1], sizeof tables[1], &tab, &cfg, NULL)) return 8;
                if (stg_game_compose(&game, &a, &draw, &live, &sa) ||
                    stg_game_compose_observed(&game, &b, &draw, &live, &sb)) return 9;
                same(&a, &b, &sa, &sb);
                memcpy(counts[mode], marks, sizeof marks);
            }
            printf("%u,%u,%u,%u,%u,%u", kind == 0 ? 16u : 8u, knobs[ki], frame,
                   sa.act.total, counts[0][8], counts[1][8]);
            for (i = 0; i < 7; i++) printf(",%u,%u", counts[0][i+1] - counts[0][i],
                                         counts[1][i+1] - counts[1][i]);
            putchar('\n');
            stg_game_step(&game, 0);
        }
    }
    return 0;
}
