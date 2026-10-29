/******************************************************************************
* @file board.c: 板级接口的 PC 实现（SDL2）
*
* @brief  PC 在这套结构里就是「另一块板子」：SDL2 开 640x480 窗口当显示，
*         一块 64MB 的模拟内存当 DDR（分区偏移与板上完全相同），
*         性能计数器折算成 100MHz 的周期数当 CLINT。
*         应用代码里不出现任何平台判断，所以这里必须把板上有的服务都实现出来，
*         哪怕只是空操作。
*
*         **场节拍在 PC 上是什么。** 板上「场」是像素域时序发生器的一根自由运行
*         时间轴：FRAME_COUNT 每个场起始加一（与 EN 无关），SWAP_REQ 是一个粘滞
*         请求位，在紧接的那一个场起始锁存、SWAP_COUNT 加一。所以 PC 侧照这个
*         模型做：`base` 在 board_init 取一次，场号是 (now − base) / step，
*         present 记下进入时的场号、等到场号变化再返回。free 的不是「第几帧画面」
*         而是「第几场」，没 present 过它照样走，这样 `Δ场 − Δ切换` 才等于掉帧数。
*
*         帧缓冲行跨固定 4096 字节，上传纹理时按同一个 pitch 交给 SDL，
*         不再另拷贝：PC 与板上的缓冲布局必须一模一样，否则逐帧
*         CRC 对比没有意义。
*
*         无窗口模式不改这里的代码：把 SDL_VIDEODRIVER 置成 dummy 再跑同一个
*         二进制即可。这样有窗与无窗确实是同一条执行路径。
*
*         **截帧**（只在 PC 上有）：没有显示器时用它看画面。
*           GFX2D_SNAPSHOT=<k,k,...>  呈现序号（第几次成功的 board_present()，从 0 起），最多 64 个
*           GFX2D_SNAPSHOT_DIR=<目录>  写到哪里，缺省当前目录
*           GFX2D_SNAPSHOT_EVERY=<n>  连续倾倒：呈现序号能被 n 整除就写一帧，n=1 即每帧都写。
*                                     做视频素材用（ffmpeg -framerate 60 -i snap_%05d.ppm），
*                                     与 GFX2D_SNAPSHOT 的指定序号表互相独立，可以同时开。
*         到了就把要呈现的那一面的 640x480 写成 PPM（文件名 snap_<k>.ppm，k 补到 5 位），
*         在 stderr 打一行 `board: snapshot k -> 路径`。不设时什么也不做，不改 board.h，也不改应用的输出。
*
*         **不等场**（只在 PC 上有）：
*           GFX2D_UNPACED=1  board_present() 照常上传纹理、计切换次数、截帧，只是不等下一个场起始就返回；
*                            board_wait_frame() 也不等。判据入口一局约一万帧，按 60 Hz 等场要三分钟，用它跑得快。
*         场号仍按墙钟自由运行，所以不等场时「场数增量」没有意义；应用不知道这回事，判据入口本来就不看它。
******************************************************************************/

#define SDL_MAIN_HANDLED        1

#include <SDL.h>
#ifdef _WIN32
#include <SDL_syswm.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"

/* 素材包文件：GFX2D_ASSETS_PAK 环境变量优先，不给时用构建时写进来的默认路径
   （mk/host.mk 由 -DGFX2D_ASSETS_PAK_DEFAULT 传，指向 Main/assets/build/assets.pak）。
   路径不进仓库文档，也不写死在源码里（改构建目录时只改 makefile 的变量）。 */
#ifndef GFX2D_ASSETS_PAK_DEFAULT
#define GFX2D_ASSETS_PAK_DEFAULT ""
#endif

/* 板级帧节拍：一场的秒数取 1/60。step 由性能计数器的频率折算，不是墙钟。 */
#define BOARD_FPS           60u

/* 缩放上限。功能键切 1x / 2x / 3x。 */
#define BOARD_SCALE_MAX     3

static uint8_t  *s_ddr;             /* 模拟的 DDR，64MB */
static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static uint64_t      s_perf_base;   /* 场 0 的性能计数器读数 */
static uint64_t      s_perf_freq;
static uint64_t      s_perf_ms;     /* 1 毫秒等于多少个计数器单位 */
static uint64_t      s_step;        /* 一场等于多少个计数器单位 */
static uint32_t      s_swaps;       /* 成功 present 过的次数 */
static int           s_scale;
static int           s_quit;
static int           s_fullscreen, s_smooth;
static int           s_escape_exits_fullscreen=1;
static int           s_window_x, s_window_y, s_window_w, s_window_h;
static unsigned      s_texture_w = FB_WIDTH, s_texture_h = FB_HEIGHT;
static uint32_t     *s_canvas[BOARD_FB_COUNT];
static unsigned      s_canvas_w[BOARD_FB_COUNT];
#define HOST_CANVAS_MAX_WIDTH 1792u

/* 串口发送额度的模拟（board.h 的 board_tx_room）。s_tx_budget 为 0 表示不限。 */
static uint32_t      s_tx_budget;   /* GFX2D_TX_BUDGET：每场补满到多少字节 */
static uint32_t      s_tx_room;     /* 本场还剩多少 */
static uint32_t      s_tx_field;    /* 上一次补额度时的场号 */
static uint32_t      s_tx_over_n;   /* board_puts 超出额度的次数 */
static uint32_t      s_tx_over_bytes;

/* 截帧（见文件头）。s_snap_n 为 0 表示不截。s_snap_every 非 0 表示连续倾倒。 */
#define SNAP_MAX            64u
static uint32_t      s_snap[SNAP_MAX];
static uint32_t      s_snap_n;
static uint32_t      s_snap_every;

/* GFX2D_UNPACED=1：呈现不等场（见文件头）。 */
static int           s_unpaced;
static const char   *s_snap_dir;

static uint32_t field_index(void);

static void die(const char *what)
{
    fprintf(stderr, "board: %s: %s\n", what, SDL_GetError());
    exit(1);
}

/* 事件泵。两件事：
   - SDL_QUIT 置 s_quit，应用经 board_should_quit 拿到；
   - 功能键切窗口大小（1x/2x/3x）。**缩放是宿主的事，不进 board_keys 的位图**：
     进了位图就会被录进录像、改变回放的按键序列，破坏确定性。 */
static void recreate_texture(void)
{
    if (s_tex != NULL) SDL_DestroyTexture(s_tex);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_XRGB8888,
                              SDL_TEXTUREACCESS_STREAMING, (int)s_texture_w, (int)s_texture_h);
    if (s_tex == NULL) die("SDL_CreateTexture");
    (void)SDL_SetTextureScaleMode(s_tex, s_smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

/* Restore saved positions only when enough of the window is still on a live
   monitor. This also handles unplugging the display that held the window. */
static void fit_window_to_display(int *x, int *y, int *w, int *h)
{
    SDL_Rect bounds;
    for (int i=0;i<SDL_GetNumVideoDisplays();++i) {
        if(SDL_GetDisplayUsableBounds(i,&bounds))continue;
        int left=*x>bounds.x?*x:bounds.x,top=*y>bounds.y?*y:bounds.y;
        int right=*x+*w<bounds.x+bounds.w?*x+*w:bounds.x+bounds.w;
        int bottom=*y+*h<bounds.y+bounds.h?*y+*h:bounds.y+bounds.h;
        if(right-left>=64 && bottom-top>=32 && *y>=bounds.y && *y+32<=bounds.y+bounds.h)return;
    }
    int display=SDL_GetWindowDisplayIndex(s_win);
    if(display<0)display=0;
    if(SDL_GetDisplayUsableBounds(display,&bounds))return;
    if(*w>bounds.w)*w=bounds.w;
    if(*h>bounds.h)*h=bounds.h;
    *x=bounds.x+(bounds.w-*w)/2;*y=bounds.y+(bounds.h-*h)/2;
}

static void fullscreen(int enabled)
{
    if (enabled == s_fullscreen) return;
    if (enabled) {
        SDL_GetWindowPosition(s_win, &s_window_x, &s_window_y);
        SDL_GetWindowSize(s_win, &s_window_w, &s_window_h);
    }
    if (SDL_SetWindowFullscreen(s_win, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
        fprintf(stderr, "board: fullscreen: %s\n", SDL_GetError());
        return;
    }
    s_fullscreen = enabled;
    if (!enabled) {
        fit_window_to_display(&s_window_x,&s_window_y,&s_window_w,&s_window_h);
        SDL_SetWindowSize(s_win, s_window_w, s_window_h);
        SDL_SetWindowPosition(s_win, s_window_x, s_window_y);
    }
}

static void pump_events(void)
{
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        if(e.type==SDL_DISPLAYEVENT && e.display.event==SDL_DISPLAYEVENT_DISCONNECTED) {
            if(s_fullscreen)fullscreen(0);
            int x,y,w,h;SDL_GetWindowPosition(s_win,&x,&y);SDL_GetWindowSize(s_win,&w,&h);
            fit_window_to_display(&x,&y,&w,&h);
            SDL_SetWindowSize(s_win,w,h);SDL_SetWindowPosition(s_win,x,y);
        }
        if (e.type == SDL_QUIT) {
            s_quit = 1;
        } else if ((e.type == SDL_RENDER_DEVICE_RESET || e.type == SDL_RENDER_TARGETS_RESET) && s_ren != NULL) {
            recreate_texture();
        } else if (e.type == SDL_KEYDOWN && s_win != NULL && !e.key.repeat) {
            int want = 0;

            if (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
                (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                fullscreen(!s_fullscreen);
                continue;
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE && s_fullscreen && s_escape_exits_fullscreen) {
                fullscreen(0);
                continue;
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F4) {
                s_smooth = !s_smooth;
                (void)SDL_SetTextureScaleMode(s_tex, s_smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
                continue;
            }

            switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_F1: want = 1; break;
            case SDL_SCANCODE_F2: want = 2; break;
            case SDL_SCANCODE_F3: want = 3; break;
            default: break;
            }
            if (want != 0) {
                fullscreen(0);
                s_scale = want;
                SDL_SetWindowSize(s_win, (int)FB_WIDTH * want, (int)FB_HEIGHT * want);
            }
        }
    }
}

/* --- 串口发送额度--------------------------------------------------------------- */
/* 板上 board_puts 忙等发送 FIFO，PC 上 stdout 不会阻塞，所以「日志不阻塞」的判据要在 PC 上
   先把额度限住才测得出来。**额度按场补，不按墙钟**：场号就是 board_frame_count() 那根时间轴，
   按墙钟补的话模拟出来的发送量与帧节拍脱钩。补满而不累加：真实的发送 FIFO 容量是固定的。 */
static void tx_report(void)
{
    fprintf(stderr, "board: tx budget %u B/field, over %u times %u bytes\n",
            (unsigned)s_tx_budget, (unsigned)s_tx_over_n, (unsigned)s_tx_over_bytes);
}

static void tx_budget_init(void)
{
    const char   *env = getenv("GFX2D_TX_BUDGET");
    char         *end = NULL;
    unsigned long v;

    s_tx_budget     = 0u;
    s_tx_over_n     = 0u;
    s_tx_over_bytes = 0u;
    if (env == NULL || env[0] == '\0')
        return;

    v = strtoul(env, &end, 10);
    if (end == env || *end != '\0' || v == 0ul || v > 65535ul) {
        fprintf(stderr, "board: GFX2D_TX_BUDGET=%s 不是 1~65535 的整数，按不限处理\n", env);
        return;
    }
    s_tx_budget = (uint32_t)v;
    s_tx_room   = s_tx_budget;
    s_tx_field  = field_index();
    atexit(tx_report);
}

/* 场号变了就补满。 */
static void tx_refill(void)
{
    uint32_t f = field_index();

    if (f != s_tx_field) {
        s_tx_field = f;
#ifndef INJECT_HOST_TX_NO_REFILL
        s_tx_room  = s_tx_budget;       /* 注错 HOST_TX_NO_REFILL 把这一句去掉 */
#endif
    }
}

uint32_t board_tx_room(void)
{
    if (s_tx_budget == 0u)
        return BOARD_TX_ROOM_UNLIMITED;
    tx_refill();
    return s_tx_room;
}

static void snap_init(void)
{
    const char *env = getenv("GFX2D_SNAPSHOT");
    const char *every = getenv("GFX2D_SNAPSHOT_EVERY");
    const char *p   = env;

    s_snap_n   = 0u;
    s_snap_dir = getenv("GFX2D_SNAPSHOT_DIR");
    if (s_snap_dir == NULL || s_snap_dir[0] == '\0')
        s_snap_dir = ".";
    if (every != NULL && every[0] != '\0') {
        unsigned long v = strtoul(every, NULL, 10);

        s_snap_every = (uint32_t)v;
    }
    if (env == NULL)
        return;

    while (*p != '\0' && s_snap_n < SNAP_MAX) {
        char         *end = NULL;
        unsigned long v = strtoul(p, &end, 10);

        if (end == p) {
            fprintf(stderr, "board: GFX2D_SNAPSHOT=%s 里有不是数字的项，后面的不截\n", env);
            break;
        }
        s_snap[s_snap_n++] = (uint32_t)v;
        p = end;
        if (*p == ',')
            p++;
    }
}

/* 把要呈现的那一面的 640x480 写成 PPM。帧缓冲是 XRGB8888（0x00RRGGBB，小端存放）。
   每行先折进一个行缓冲再一次 fwrite：连续倾倒时逐像素 fputc 的开销足以把呈现拖离 60 Hz。 */
static void snap_write(const uint32_t *fb, uint32_t idx)
{
    char     path[512];
    FILE    *fp;
    uint32_t x, y;
    uint8_t  row[FB_WIDTH * 3u];

    snprintf(path, sizeof path, "%s/snap_%05u.ppm", s_snap_dir, (unsigned)idx);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "board: snapshot %u 写不了 %s\n", idx, path);
        return;
    }
    fprintf(fp, "P6\n%u %u\n255\n", (unsigned)FB_WIDTH, (unsigned)FB_HEIGHT);
    for (y = 0u; y < FB_HEIGHT; y++) {
        const uint32_t *line = &fb[y * FB_ROW_PIXELS];

        for (x = 0u; x < FB_WIDTH; x++) {
            const uint32_t v = line[x];

            row[x * 3u]       = (uint8_t)((v >> 16) & 0xFFu);
            row[x * 3u + 1u]  = (uint8_t)((v >> 8) & 0xFFu);
            row[x * 3u + 2u]  = (uint8_t)(v & 0xFFu);
        }
        fwrite(row, 1u, sizeof row, fp);
    }
    fclose(fp);
    fprintf(stderr, "board: snapshot %u -> %s\n", idx, path);
}

/* 连续倾倒：呈现序号能被 s_snap_every 整除就写一帧。 */
static void snap_every_maybe(const uint32_t *fb, uint32_t idx)
{
    if (s_snap_every != 0u && idx % s_snap_every == 0u)
        snap_write(fb, idx);
}

static void snap_maybe(const uint32_t *fb, uint32_t idx)
{
    uint32_t i;

    for (i = 0u; i < s_snap_n; i++) {
        if (s_snap[i] == idx)
            break;
    }
    if (i == s_snap_n)
        return;

    snap_write(fb, idx);
}

void board_init(void)
{
    s_ddr = (uint8_t *)calloc(1u, (size_t)DDR_SIM_BYTES);
    if (s_ddr == NULL) {
        fprintf(stderr, "board: 分配 %u 字节模拟 DDR 失败\n", (unsigned)DDR_SIM_BYTES);
        exit(1);
    }

    SDL_SetMainReady();
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        die("SDL_Init");

    s_win = SDL_CreateWindow("MYRIAD | F11 fullscreen | F4 smooth/pixel",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             (int)FB_WIDTH, (int)FB_HEIGHT, SDL_WINDOW_RESIZABLE);
    if (s_win == NULL)
        die("SDL_CreateWindow");

    /* 不带 SDL_RENDERER_PRESENTVSYNC：PC 的场节拍由下面的时间轴给，
       交给显示器的刷新率之后，高刷屏上量出来的帧率就不是 60 了。 */
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (s_ren == NULL)
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);   /* 无 3D 驱动 / dummy 驱动 */
    if (s_ren == NULL)
        die("SDL_CreateRenderer");
    if (SDL_RenderSetLogicalSize(s_ren, (int)FB_WIDTH, (int)FB_HEIGHT) != 0)
        die("SDL_RenderSetLogicalSize");
    SDL_SetWindowMinimumSize(s_win, 320, 240);

    /* 缩放用最近邻。SDL 对这个 hint 的默认值是**未设**，
       所以必须显式写 "0"；它在建纹理时被读走，之后改没有用。 */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    {
        const char *filter=getenv("GFX2D_SCALE_FILTER");
        s_smooth=filter && strcmp(filter,"linear")==0;
    }
    recreate_texture();
    s_fullscreen = 0;

    s_perf_freq = (uint64_t)SDL_GetPerformanceFrequency();
    s_perf_ms   = s_perf_freq / 1000u;
    s_step      = s_perf_freq / (uint64_t)BOARD_FPS;
    if (s_perf_freq == 0u || s_perf_ms == 0u || s_step == 0u) {
        fprintf(stderr, "board: 性能计数器频率 %llu 折算不出一场，本平台不适用\n",
                (unsigned long long)s_perf_freq);
        exit(1);
    }

    s_perf_base = (uint64_t)SDL_GetPerformanceCounter();
    s_swaps     = 0u;
    s_scale     = 1;
    s_quit      = 0;

    tx_budget_init();
    snap_init();
    {
        const char *env = getenv("GFX2D_UNPACED");

        s_unpaced = (env != NULL && strcmp(env, "1") == 0);
        if (s_unpaced)
            fprintf(stderr, "board: GFX2D_UNPACED=1，呈现不等场\n");
    }
    {
        const char *start_fullscreen=getenv("GFX2D_FULLSCREEN");
        if(start_fullscreen && strcmp(start_fullscreen,"1")==0)fullscreen(1);
    }
}

board_mem board_region(board_region_id id)
{
    board_mem m;

    if (id >= REGION_COUNT) {
        m.ptr  = NULL;
        m.size = 0u;
        return m;
    }
    m.ptr  = s_ddr + g_mem_regions[id].offset;
    m.size = g_mem_regions[id].size;
    return m;
}

/* --- 素材分区--------------------------------------------------------------------- */
/* PC 上素材分区就是 assets.pak 这一个文件：offset 是文件偏移。
   每次调用开一次文件，读两段（包头、正文）就是开两次，
   比起把整包先读进内存再校验，这样不会在包头就坏掉时读进一个来路不明的长度。 */
static const char *asset_pak_path(void)
{
    const char *env = getenv("GFX2D_ASSETS_PAK");

    if (env != NULL && env[0] != '\0')
        return env;
    return GFX2D_ASSETS_PAK_DEFAULT;
}

int board_asset_read(uint32_t offset, void *dst, uint32_t bytes)
{
    const char *path = asset_pak_path();
    FILE       *f;
    size_t      got;

    if (path[0] == '\0')
        return BOARD_ASSET_E_PATH;      /* 构建时没给默认路径，也没设环境变量 */
    if (dst == NULL)
        return BOARD_ASSET_E_RANGE;
    if (offset > BOARD_ASSET_MAX_BYTES || bytes > BOARD_ASSET_MAX_BYTES - offset)
        return BOARD_ASSET_E_RANGE;

    f = fopen(path, "rb");
    if (f == NULL)
        return BOARD_ASSET_E_IO;
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return BOARD_ASSET_E_IO;
    }
    got = fread(dst, 1u, (size_t)bytes, f);
    fclose(f);
    if (got != (size_t)bytes)
        return BOARD_ASSET_E_IO;        /* 文件比包头里写的短 */
    return BOARD_ASSET_OK;
}

uint64_t board_time_ms(void)
{
    uint64_t d=SDL_GetPerformanceCounter()-s_perf_base;
    return (d/s_perf_freq)*1000u+(d%s_perf_freq)*1000u/s_perf_freq;
}
void board_cpu_write_complete(void) { }
uint32_t board_cycles(void)
{
    uint64_t dt = (uint64_t)SDL_GetPerformanceCounter() - s_perf_base;

#ifdef INJECT_HOST_CYCLES_NAIVE
    /* 注错：直接相乘。本机 SDL 的计数器频率是 1e9，dt * 1e8 在 dt 约 1.845e11
       （184.5 秒）时越过 2^64，之后返回的数值乱跳，不再是任何有意义的周期数。
       用它做「跑满 200 秒」的对照。 */
    return (uint32_t)((dt * (uint64_t)BOARD_HZ) / s_perf_freq);
#else
    /* 先拆成整秒与余数再乘，中间量不会越界：
       whole * BOARD_HZ 的上限约 1.8e18（要跑几千年才到），
       part * BOARD_HZ 的上限是 1e17，都远在 2^64 之内。
       结果按 32 位回绕，契约见 board.h。 */
    {
        uint64_t whole = dt / s_perf_freq;      /* 整秒 */
        uint64_t part  = dt % s_perf_freq;      /* 不足一秒的余数 */

        return (uint32_t)(whole * (uint64_t)BOARD_HZ +
                          (part * (uint64_t)BOARD_HZ) / s_perf_freq);
    }
#endif
}

void board_puts(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);         /* 每次输出都落地：串口采集端不会漏行 */

    /* 限了额度时从额度里扣；超出的照样打出来，只计数（board.h 的 board_tx_room）。 */
    if (s_tx_budget != 0u) {
        uint32_t n = (uint32_t)strlen(s);

        tx_refill();
        if (n > s_tx_room) {
            s_tx_over_n++;
            s_tx_over_bytes += n - s_tx_room;
            s_tx_room = 0u;
        } else {
            s_tx_room -= n;
        }
    }
}

/* **缩放键不在这里**：窗口大小是宿主的事，缩放键在 pump_events 里被吃掉。
   加进来就会进录像、改变回放的按键序列。 */
void board_input_window_policy(int enabled) { s_escape_exits_fullscreen=enabled!=0; }

uint32_t board_input_raw(uint32_t *physical)
{
    if(physical)*physical=0;
    const Uint8 *k;
    uint32_t     bits = 0u;

    /* 顺手泵一次事件：关窗那一刻要能被看到，哪怕这一帧没有 present。
       （SDL_PumpEvents 只更新键盘状态，SDL_QUIT 要 SDL_PollEvent 才取出来。） */
    pump_events();

    SDL_PumpEvents();
#ifdef _WIN32
    /* SDL's focus event may lag a native activation. The native foreground
       owner is authoritative and also prevents keys from another app leaking. */
    SDL_SysWMinfo window_info;SDL_VERSION(&window_info.version);
    if (!SDL_GetWindowWMInfo(s_win,&window_info) ||
        GetForegroundWindow()!=window_info.info.win.window) return 0u;
#else
    if (!(SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS)) return 0u;
#endif
    k = SDL_GetKeyboardState(NULL);
    if (k == NULL)
        return 0u;

    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W])        bits |= BOARD_KEY_UP;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S])      bits |= BOARD_KEY_DOWN;
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A])      bits |= BOARD_KEY_LEFT;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D])     bits |= BOARD_KEY_RIGHT;
    if (k[SDL_SCANCODE_SPACE])                          bits |= BOARD_KEY_BOMB;
    if (k[SDL_SCANCODE_LSHIFT])                         bits |= BOARD_KEY_SLOW;
    if (k[SDL_SCANCODE_TAB])                            bits |= BOARD_KEY_SWAP;
    if ((k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]) && !(SDL_GetModState()&KMOD_ALT))
        bits |= BOARD_KEY_CONFIRM;
    if (k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE]) bits |= BOARD_KEY_BACK;
    if (k[SDL_SCANCODE_C])                              bits |= BOARD_KEY_SCENE;
    if (k[SDL_SCANCODE_1])                              bits |= BOARD_KEY_MODE1;
    if (k[SDL_SCANCODE_2])                              bits |= BOARD_KEY_MODE2;
    if (k[SDL_SCANCODE_3])                              bits |= BOARD_KEY_MODE3;
    if (k[SDL_SCANCODE_4])                              bits |= BOARD_KEY_MODE1 | BOARD_KEY_MODE2 |
                                                            BOARD_KEY_MODE3;   /* 组合=模式4 */
    return bits;
}

uint32_t *board_fb(int index)
{
    if (index < 0 || index >= BOARD_FB_COUNT)
        return NULL;
    return (uint32_t *)(s_ddr + g_mem_regions[REGION_FB0 + index].offset);
}

/* 场号。自由运行，与有没有 present 无关（板上 FRAME_COUNT 也是这样）。 */
static uint32_t field_index(void)
{
    return (uint32_t)(((uint64_t)SDL_GetPerformanceCounter() - s_perf_base) / s_step);
}

/* 等到场号大于 f0，也就是「下一个场起始」。板上写 SWAP_REQ 之后等的是同一个东西。

   睡法：剩下超过 1 毫秒就 SDL_Delay 掉整毫秒减一，最后一毫秒自旋。
   本机 SDL_Delay(1) 实测 1058~1149 微秒，指望它精确到微秒会把每一帧都睡过头
   （实测最大值 17.13 毫秒对一场 16.667 毫秒，偏差 3% 以内）。 */
static void wait_next_field(uint32_t f0)
{
    uint64_t target = s_perf_base + ((uint64_t)f0 + 1u) * s_step;

    for (;;) {
        uint64_t now = (uint64_t)SDL_GetPerformanceCounter();
        uint64_t left;

        if (s_quit)
            return;
        if (now >= target)
            return;

        left = target - now;
        if (left > s_perf_ms) {
            uint64_t ms = left / s_perf_ms;

            SDL_Delay((ms > 1u) ? (Uint32)(ms - 1u) : 1u);
        }
    }
}

static int s_present_pending;
static uint32_t s_present_ticket, s_present_field;

int board_present_request(int index, uint32_t *ticket)
{
    board_frame frame = {board_fb(index),FB_WIDTH,FB_HEIGHT,FB_ROW_PIXELS};
    return board_present_frame_request(index,&frame,ticket);
}

static unsigned canvas_width_for_output(int width,int height)
{
    if(width<=0 || height<=0)return FB_WIDTH;
    uint64_t desired=((uint64_t)(unsigned)width*FB_HEIGHT+(unsigned)height/2u)/(unsigned)height;
    if(desired<=FB_WIDTH)return FB_WIDTH;
    return desired>HOST_CANVAS_MAX_WIDTH?HOST_CANVAS_MAX_WIDTH:(unsigned)desired;
}

int board_frame_acquire(int index, int adaptive, board_frame *frame)
{
    int width=0,height=0;
    if(!frame || index<0 || index>=BOARD_FB_COUNT)return -1;
    pump_events();
    *frame=(board_frame){board_fb(index),FB_WIDTH,FB_HEIGHT,FB_ROW_PIXELS};
    const char *aspect=SDL_getenv("GFX2D_ASPECT");
    if(!adaptive || (aspect && strcmp(aspect,"fixed")==0))return 0;
    if(SDL_GetRendererOutputSize(s_ren,&width,&height) || width<=0 || height<=0)return 0;
    unsigned canvas_w=canvas_width_for_output(width,height);
    if(canvas_w==FB_WIDTH)return 0;
    if(s_canvas_w[index]!=canvas_w) {
        uint32_t *pixels=(uint32_t *)calloc((size_t)canvas_w*FB_HEIGHT,sizeof(uint32_t));
        if(!pixels)return -1;
        free(s_canvas[index]);s_canvas[index]=pixels;s_canvas_w[index]=canvas_w;
    }
    *frame=(board_frame){s_canvas[index],canvas_w,FB_HEIGHT,canvas_w};
    return 0;
}

int board_present_frame_request(int index, const board_frame *frame, uint32_t *ticket)
{
    uint32_t *fb;

    if (ticket == NULL || index < 0 || index >= BOARD_FB_COUNT || s_present_pending ||
        !frame || !frame->pixels || frame->height!=FB_HEIGHT || frame->width<FB_WIDTH ||
        frame->width>HOST_CANVAS_MAX_WIDTH || frame->stride<frame->width)
        return -1;
    int fixed=frame->pixels==board_fb(index) && frame->width==FB_WIDTH && frame->stride==FB_ROW_PIXELS;
    int canvas=frame->width>FB_WIDTH && frame->pixels==s_canvas[index] &&
               frame->width==s_canvas_w[index] && frame->stride==frame->width;
    if(!fixed && !canvas)return -1;
    *ticket = s_swaps;
    if (s_quit)
        return 0;

    fb = frame->pixels;
    if(s_texture_w!=frame->width || s_texture_h!=frame->height) {
        s_texture_w=frame->width;s_texture_h=frame->height;
        recreate_texture();
        if(SDL_RenderSetLogicalSize(s_ren,(int)s_texture_w,(int)s_texture_h))return -1;
    }
    /* Fixed captures keep their original 640x480 identity. Adaptive captures
       use the actual canvas, including the right-anchored HUD. */
    if(frame->width!=FB_WIDTH) {
        int capture=s_snap_every && s_swaps%s_snap_every==0;
        for(unsigned i=0;i<s_snap_n;++i)capture|=s_snap[i]==s_swaps;
        if(capture) {
            char path[512];snprintf(path,sizeof path,"%s/snap_%05u.ppm",s_snap_dir,(unsigned)s_swaps);
            FILE *file=fopen(path,"wb");
            if(file) {
                fprintf(file,"P6\n%u %u\n255\n",frame->width,frame->height);
                for(unsigned y=0;y<frame->height;++y)for(unsigned x=0;x<frame->width;++x) {
                    uint32_t pixel=fb[(size_t)y*frame->stride+x];
                    unsigned char rgb[3]={(unsigned char)(pixel>>16),(unsigned char)(pixel>>8),(unsigned char)pixel};
                    (void)fwrite(rgb,1,3,file);
                }
                fclose(file);
            }
        }
    }
    if (frame->width==FB_WIDTH && s_snap_every != 0u)
        snap_every_maybe(fb, s_swaps);
    if (frame->width==FB_WIDTH && s_snap_n != 0u)
        snap_maybe(fb, s_swaps);
    if (fb != NULL) {
        SDL_UpdateTexture(s_tex, NULL, fb, (int)(frame->stride*sizeof(uint32_t)));
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
    }

    /* 关窗按钮可能在这一次等待里被按下，泵一次再看：看到了就立即返回，
       这一帧的场数增量是 0，应用据此知道「不是掉帧，是退出」。 */
    pump_events();
    if (s_quit)
        return 0;

    s_present_ticket = s_swaps;
    s_present_field = field_index();
    s_present_pending = 1;
    return 0;
}

int board_present_finished(uint32_t ticket)
{
    if (s_quit)
        return 1;
    if (s_present_pending && ticket == s_present_ticket &&
        (s_unpaced || field_index() != s_present_field)) {
        s_swaps++;
        s_present_pending = 0;
    }
    return s_swaps != ticket;
}

int board_present_wait(uint32_t ticket)
{
    while (!board_present_finished(ticket))
        wait_next_field(s_present_field);
    return 0;
}

uint32_t board_present_retries(void) { return 0u; }

void board_present(int index)
{
    uint32_t ticket;
    if (board_present_request(index, &ticket) == 0)
        (void)board_present_wait(ticket);
}

int board_irq_init(void) { return 0; }
int board_irq_enabled(void) { return 0; }
void board_engine_irq_enable(void) { }
uint32_t board_irq_frame_events(void) { return 0u; }
uint32_t board_irq_engine_events(void) { return 0u; }
uint32_t board_irq_faults(void) { return 0u; }
int board_display_diag(uint32_t *frame, uint32_t *fifo_min,
                       uint32_t *zero_hits, uint32_t *under_beats)
{
    if (frame == NULL || fifo_min == NULL || zero_hits == NULL || under_beats == NULL)
        return -1;
    *frame = 0u;
    *fifo_min = 0u;
    *zero_hits = 0u;
    *under_beats = 0u;
    return 0;
}

void board_wait_frame(void)
{
    uint32_t f0;

    if (s_quit || s_unpaced)
        return;

    f0 = field_index();
    wait_next_field(f0);
}

uint32_t board_frame_count(void)
{
    return field_index();
}

uint32_t board_swap_count(void)
{
    return s_swaps;
}

int board_should_quit(void)
{
    return s_quit;
}

/* 数据缓存只有那颗 VexRiscv 有，PC 上都是空操作。
   函数必须存在：应用与 lib 调的是同一套接口。 */
void board_dcache_invalidate_all(void)
{
}

void board_dcache_invalidate_range(const void *addr, uint32_t bytes)
{
    (void)addr;
    (void)bytes;
}
