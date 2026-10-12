/******************************************************************************
* @file check_sw4_scale.c: 缩放用最近邻的主机侧探针
*
* @brief  PC 平台层「缩放」判据的前一半：1x/2x/3x 三种窗口下画面内容相同
*         由判据程序自己判，而「放大用的是最近邻」这件事在应用里看不出来
*         （应用只拿到帧缓冲指针，采样方式在 bsp/host 建的纹理上），所以单独做一支探针。
*
*         做法：把 8x8 的黑白棋盘放大 80 倍铺满 640x480，读回第 40 行。
*         最近邻下第 79、80 两个相邻像素是黑白硬边（一个源像素正好铺满 80 个屏幕像素）；
*         线性插值下边界两侧都是中间值。`SDL_RenderReadPixels` 读的是渲染目标的背面，
*         软件渲染器与 dummy 驱动下都可用。
*
*         退出码 0 表示通过；末尾打 `fail 0` 与 `done`。
*         编译与运行见 Main/tools/check/README.md。
******************************************************************************/

#define SDL_MAIN_HANDLED 1

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail;

static void ck(const char *tag, int ok)
{
    printf("%s %s\n", tag, ok ? "ok" : "FAIL");
    if (!ok)
        g_fail++;
}

/* 按给定的缩放质量铺一次棋盘，读回第 ROW 行，把 xs 里那几个列取出来。 */
static int sample_row(SDL_Renderer *ren, const char *quality, int row,
                      const int *xs, uint32_t *out, int n)
{
    static uint32_t src[8 * 8];
    uint32_t       *dst;
    SDL_Texture    *tex;
    int             x, y, rc;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, quality);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            src[y * 8 + x] = (((x ^ y) & 1) ? 0x00FFFFFFu : 0x00000000u);

    /* 纹理在 hint 被设好之后才建：缩放质量是建纹理时读走的 */
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 8, 8);
    if (tex == NULL) {
        printf("CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }
    (void)SDL_UpdateTexture(tex, NULL, src, 8 * 4);

    dst = (uint32_t *)calloc(640u * 480u, 4u);
    if (dst == NULL) {
        SDL_DestroyTexture(tex);
        return -1;
    }

    (void)SDL_RenderClear(ren);
    (void)SDL_RenderCopy(ren, tex, NULL, NULL);
    rc = SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_XRGB8888, dst, 640 * 4);
    if (rc != 0) {
        printf("RenderReadPixels failed: %s\n", SDL_GetError());
        free(dst);
        SDL_DestroyTexture(tex);
        return -1;
    }

    for (x = 0; x < n; x++)
        out[x] = dst[row * 640 + xs[x]] & 0x00FFFFFFu;

    free(dst);
    SDL_DestroyTexture(tex);
    return 0;
}

int main(void)
{
    SDL_Window   *win;
    SDL_Renderer *ren;
    uint32_t      nearest[5];
    uint32_t      linear[5];
    const int     probe_x[5] = { 79, 80, 81, 159, 160 };
    int           i;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("video driver = %s\n", SDL_GetCurrentVideoDriver());

    win = SDL_CreateWindow("check_sw4_scale", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           640, 480, 0);
    if (win == NULL) {
        printf("CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (ren == NULL) {
        printf("CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    /* hint 的默认值是未设。先确认这一点，再确认设 "0" 与设 "1" 结果不同：
       两条合起来才说明「最近邻是显式设定的结果，不是碰巧」。 */
    ck("S1 hint 默认未设", SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY) == NULL);

    if (sample_row(ren, "0", 40, probe_x, nearest, 5) != 0 ||
        sample_row(ren, "1", 40, probe_x, linear, 5) != 0) {
        printf("fail 1\ndone\n");
        return 1;
    }

    printf("hint 0 (最近邻):");
    for (i = 0; i < 5; i++)
        printf(" x=%d:0x%06x", probe_x[i], nearest[i]);
    printf("\n");
    printf("hint 1 (线性)  :");
    for (i = 0; i < 5; i++)
        printf(" x=%d:0x%06x", probe_x[i], linear[i]);
    printf("\n");

    ck("S2 hint 0 边界是硬边",
       nearest[0] == 0x000000u && nearest[1] == 0xFFFFFFu && nearest[2] == 0xFFFFFFu);
    ck("S3 hint 0 下一个源像素也是硬边",
       nearest[3] == 0xFFFFFFu && nearest[4] == 0x000000u);
    ck("S4 hint 1 边界被插值",
       linear[0] != nearest[0] || linear[1] != nearest[1]);
    ck("S5 两个 hint 的结果不同",
       nearest[0] != linear[0] || nearest[1] != linear[1]);

    /* 三种窗口尺寸都要设得下去、读得回来 */
    {
        const int scale[3] = { 1, 2, 3 };
        int       bad = 0;

        for (i = 0; i < 3; i++) {
            int w = 0, h = 0;
            const int want_w = 640 * scale[i];
            const int want_h = 480 * scale[i];

            SDL_SetWindowSize(win, want_w, want_h);
            SDL_PumpEvents();
            SDL_GetWindowSize(win, &w, &h);
            printf("window %dx%d 读回 %dx%d\n", want_w, want_h, w, h);
            if (w != want_w || h != want_h)
                bad++;
        }
        ck("S6 三种窗口尺寸都设得下去", bad == 0);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    printf("fail %d\n", g_fail);
    printf("done\n");
    return (g_fail == 0) ? 0 : 1;
}
