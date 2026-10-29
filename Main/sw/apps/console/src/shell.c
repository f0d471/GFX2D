/******************************************************************************
* @file shell.c: 菜单应用（SW10）
*
* @brief  画面：深色底 + 标题图（premult ALPHA，同游戏标题的画法）+ 纵向列表
*         （选中行青字库 + 光标小飞船，未选中白字库）+ 页脚键位提示 + 版本/fps。
*         素材全部来自同一份 pak（FONT/FONT_CYAN/TITLE/SHIPS），表面槽用 stg
*         那一套（stg_runtime_surfaces_stg 一次登记全）。
*
*         键位（只在菜单内）：上下移动光标、BOMB 启动选中项、
*         数字 1/2/3/4 由框架按稳定 ID 处理；菜单仅枚举可启动内容。
*         绘制与错误处理在两端共用；运行平台决定 CPU 或引擎执行。
*         第一帧打一行 `menu crc <hex> sel <n>`：板上判据的渲染锚点。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>


#include "core/crc32.h"
#include "core/format.h"

#include "gfx/gfx.h"

#include "assets.h"
#include "stg_slots.h"
#include "game/stg_text.h"

#include "app_port.h"

#include "console_views.h"
#include "shell.h"

/* 与游戏一致的字体几何（sw6_hud 的那套；青字库同规格） */
static const stg_font s_font = {
    STG_SLOT_FONT, ASSET_FONT8X16_CELL_W, ASSET_FONT8X16_CELL_H,
    ASSET_FONT8X16_COLS, 0x20u, ASSET_FONT8X16_CELLS, 0x00000000u,
};
static const stg_font s_font_cyan = {
    STG_SLOT_FONT_CYAN, ASSET_FONT_CYAN_CELL_W, ASSET_FONT_CYAN_CELL_H,
    ASSET_FONT_CYAN_COLS, 0x20u, ASSET_FONT_CYAN_CELLS, ASSET_FONT_CYAN_KEY,
};

#define MENU_BG      0xFF100E1Au      /* ARGB：近黑深蓝 */
#define MENU_Y0      204
#define MENU_STEP   32
#define CURSOR_X    196
#define NAME_X      244
#define DESC_X      420
#define FOOT_Y      440

static uint32_t s_sel;             /* 光标条目 */
static uint32_t s_prev_keys;
static int      s_first_logged;
static int      s_hw;              /* 1=引擎渲染，0=SW 兜底（引擎不在的老位流） */

static int shell_enter(appfw_ctx *c)
{
    s_hw=app_port_hardware();
    if(console_view_prepare(c,CONSOLE_VIEW_STG,s_hw)!=0)return -1;
    s_prev_keys    = 0u;
    s_first_logged = 0;
    return 0;
}

static int shell_leave(appfw_ctx *c)
{
    (void)c;    /* renderer_run 同步等表执行完才返回，没有在途的引擎表 */
    return 0;
}

static int shell_frame(appfw_ctx *c, uint32_t keys, uint32_t frame)
{
    const uint32_t pressed = keys & ~s_prev_keys;
    int dst;
    uint32_t       count, i;
    gfx_list       l;
    gfx_rect       all;
    uint32_t       bad = 0u;

    s_prev_keys = keys;

    /* --- 输入 --- */
    (void)appfw_apps(&count);
    if(!count)return -1;
    if(s_sel>=count)s_sel=0;
    if (pressed & PLATFORM_KEY_UP)
        s_sel = (s_sel == 0u) ? count - 1u : s_sel - 1u;
    if (pressed & PLATFORM_KEY_DOWN)
        s_sel = (s_sel + 1u) % count;
    if (pressed & (PLATFORM_KEY_ACTION|PLATFORM_KEY_CONFIRM))
        appfw_request(appfw_app_at(s_sel));

    /* --- 组帧 --- */
    {
    all.x0 = 0;
    all.y0 = 0;
    all.x1 = (int32_t)CONSOLE_WIDTH - 1;
    all.y1 = (int32_t)CONSOLE_HEIGHT - 1;

    if(console_view_begin(c,&l,&dst)!=0)return -1;
    bad += (uint32_t)(gfx_set_clip(&l, all) != 0);
    bad += (uint32_t)(gfx_fill(&l, dst, all, MENU_BG, GFX_EQ_COPY, 0u) != 0);

    /* 标题：premult ALPHA，画法同游戏标题 */
    bad += (uint32_t)(gfx_blit(&l, STG_SLOT_TITLE, 0, 0, dst,
                               ((int)CONSOLE_WIDTH - (int)ASSET_TITLE_W) / 2, 44,
                               (int)ASSET_TITLE_W, (int)ASSET_TITLE_H-16,
                               GFX_EQ_ALPHA, 0u, 0, 0u) != 0);

    /* 列表：光标小飞船（ships 第 0 格，黑键透明）+ 名称 + 说明 */
    {
        const appfw_app *const *apps = appfw_apps(&count);
        char sel_line[48];
        core_out o;

        for (i = 0u; i < count; i++) {
            const int          y = MENU_Y0 + (int)(i * MENU_STEP);
            const stg_font    *f = (i == s_sel) ? &s_font_cyan : &s_font;

            if (i == s_sel) {
                bad += (uint32_t)(gfx_blit(&l, STG_SLOT_SHIPS, 0, 0, dst,
                                           CURSOR_X,
                                           y - ((int)ASSET_SHIPS_CELL_H - (int)f->cell_h) / 2,
                                           (int)ASSET_SHIPS_CELL_W, (int)ASSET_SHIPS_CELL_H,
                                           GFX_EQ_COPY, 0u, 1, ASSET_SHIPS_KEY) != 0);
            }
            core_out_init(&o,sel_line,sizeof sel_line);
            if(apps[i]->id<=APPFW_R4)core_out_u32(&o,apps[i]->id);
            else core_out_ch(&o,' ');
            core_out_ch(&o,' ');core_out_str(&o,apps[i]->desc);
            bad += stg_text_draw(&l,f,dst,NAME_X,y,sel_line,NULL);
            if(!appfw_available(apps[i]))
                bad += stg_text_draw(&l,&s_font,dst,NAME_X,y+16,"UNAVAILABLE",NULL);
        }

        /* 选中条目号（判据锚点：菜单行） */
        core_out_init(&o, sel_line, (uint32_t)sizeof sel_line);
        core_out_str(&o, "SELECT APPLICATION");
        bad += stg_text_draw(&l,&s_font,dst,NAME_X,176,sel_line,NULL);
    }

    /* 页脚、版本与 fps */
    bad += stg_text_draw(&l, &s_font, dst, 76, FOOT_Y,
                         "1-4 APP  ENTER/SPACE OK  ESC BACK  C SCENE", NULL);
    bad += stg_text_draw(&l,&s_font,dst,76,456,"BOARD: LEFT+RIGHT OK  UP+DOWN BACK",NULL);
    bad += stg_text_draw(&l, &s_font, dst, 508, 8, "CONSOLE V2", NULL);
    {
        char     line[16];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_u32(&o, c->fps_milli/1000u);
        bad += stg_text_draw(&l, &s_font, dst, 604, FOOT_Y, line, NULL);
    }

    bad += (uint32_t)(gfx_list_end(&l) != 0);
    }

    if(bad)return appfw_error("menu build",-1);
    if(console_view_submit(c,&l,dst,s_hw)!=0)return -1;

    if (!s_first_logged) {
        char     buf[96];
        core_out o;

        s_first_logged = 1;
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "menu crc ");
        core_out_hex32(&o, console_view_crc(c));
        core_out_str(&o, " sel ");
        core_out_u32(&o, s_sel);
        core_out_str(&o, "\r\n");
        appfw_log(buf);
    }

    (void)frame;
    return APPFW_FRAME_OK;
}

const appfw_app g_app_menu = {
    APPFW_MENU,"menu", "SELECT APPLICATION",
    shell_enter,
    shell_leave,
    shell_frame,
};

const appfw_app *shell_app(void)
{
    return &g_app_menu;
}
