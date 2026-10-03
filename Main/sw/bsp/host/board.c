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
*         模型与实测见 Engineering/TechReports/sw/03-PC平台层与帧节拍.md 3.4 节与 3.1 节。
*
*         帧缓冲行跨固定 4096 字节，上传纹理时按同一个 pitch 交给 SDL，
*         不再另拷贝：PC 与板上的缓冲布局必须一模一样，否则 SW9 的逐帧
*         CRC 对比没有意义。
*
*         无窗口模式不改这里的代码：把 SDL_VIDEODRIVER 置成 dummy 再跑同一个
*         二进制即可（sw 第 03 章 3.2 节的 D3、3.5 节）。这样有窗与无窗确实是同一条执行路径。
*
*         **截帧**（只在 PC 上有，sw 第 06 章 3.9 节）：没有显示器时用它看画面。
*           GFX2D_SNAPSHOT=<k,k,...>  呈现序号（第几次成功的 board_present()，从 0 起），最多 64 个
*           GFX2D_SNAPSHOT_DIR=<目录>  写到哪里，缺省当前目录
*         到了就把要呈现的那一面的 640x480 写成 PPM（P6，文件名 snap_<k>.ppm，k 补到 5 位），
*         在 stderr 打一行 `board: snapshot k -> 路径`。不设时什么也不做，不改 board.h，也不改应用的输出。
*
*         **不等场**（只在 PC 上有，sw 第 06 章 3.13 节）：
*           GFX2D_UNPACED=1  board_present() 照常上传纹理、计切换次数、截帧，只是不等下一个场起始就返回；
*                            board_wait_frame() 也不等。判据入口一局约一万帧，按 60 Hz 等场要三分钟，用它跑得快。
*         场号仍按墙钟自由运行，所以不等场时「场数增量」没有意义；应用不知道这回事，判据入口本来就不看它。
******************************************************************************/

#define SDL_MAIN_HANDLED        1

#include <SDL.h>
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

/* 缩放上限。F1~F3 切 1x / 2x / 3x。 */
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

/* 串口发送额度的模拟（sw 第 07 章 3.3 节，board.h 的 board_tx_room）。s_tx_budget 为 0 表示不限。 */
static uint32_t      s_tx_budget;   /* GFX2D_TX_BUDGET：每场补满到多少字节 */
static uint32_t      s_tx_room;     /* 本场还剩多少 */
static uint32_t      s_tx_field;    /* 上一次补额度时的场号 */
static uint32_t      s_tx_over_n;   /* board_puts 超出额度的次数 */
static uint32_t      s_tx_over_bytes;

/* 截帧（见文件头）。s_snap_n 为 0 表示不截。 */
#define SNAP_MAX            64u
static uint32_t      s_snap[SNAP_MAX];
static uint32_t      s_snap_n;

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
   - F1~F3 切窗口大小。**缩放是宿主的事，不进 board_keys 的位图**：
     进了位图就会被录进录像、改变回放的按键序列，破坏确定性。 */
static void pump_events(void)
{
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            s_quit = 1;
        } else if (e.type == SDL_KEYDOWN && s_win != NULL) {
            int want = 0;

            switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_F1: want = 1; break;
            case SDL_SCANCODE_F2: want = 2; break;
            case SDL_SCANCODE_F3: want = 3; break;
            default: break;
            }
            if (want != 0 && want != s_scale) {
                s_scale = want;
                SDL_SetWindowSize(s_win, (int)FB_WIDTH * want, (int)FB_HEIGHT * want);
            }
        }
    }
}

/* --- 串口发送额度（sw 第 07 章 3.3 节）------------------------------------------- */
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
    const char *p   = env;

    s_snap_n   = 0u;
    s_snap_dir = getenv("GFX2D_SNAPSHOT_DIR");
    if (s_snap_dir == NULL || s_snap_dir[0] == '\0')
        s_snap_dir = ".";
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

/* 呈现序号 idx 在表里就把 fb 写成 PPM。帧缓冲是 XRGB8888（0x00RRGGBB，小端存放）。 */
static void snap_maybe(const uint32_t *fb, uint32_t idx)
{
    char     path[512];
    FILE    *fp;
    uint32_t i, x, y;

    for (i = 0u; i < s_snap_n; i++) {
        if (s_snap[i] == idx)
            break;
    }
    if (i == s_snap_n)
        return;

    snprintf(path, sizeof path, "%s/snap_%05u.ppm", s_snap_dir, (unsigned)idx);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "board: snapshot %u 写不了 %s\n", (unsigned)idx, path);
        return;
    }
    fprintf(fp, "P6\n%u %u\n255\n", (unsigned)FB_WIDTH, (unsigned)FB_HEIGHT);
    for (y = 0u; y < FB_HEIGHT; y++) {
        for (x = 0u; x < FB_WIDTH; x++) {
            const uint32_t v = fb[y * FB_ROW_PIXELS + x];

            fputc((int)((v >> 16) & 0xFFu), fp);
            fputc((int)((v >> 8) & 0xFFu), fp);
            fputc((int)(v & 0xFFu), fp);
        }
    }
    fclose(fp);
    fprintf(stderr, "board: snapshot %u -> %s\n", (unsigned)idx, path);
}

void board_init(void)
{
    s_ddr = (uint8_t *)calloc(1u, (size_t)DDR_SIM_BYTES);
    if (s_ddr == NULL) {
        fprintf(stderr, "board: 分配 %u 字节模拟 DDR 失败\n", (unsigned)DDR_SIM_BYTES);
        exit(1);
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        die("SDL_Init");

    s_win = SDL_CreateWindow("gfx2d PC (SW4)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             (int)FB_WIDTH, (int)FB_HEIGHT, 0);
    if (s_win == NULL)
        die("SDL_CreateWindow");

    /* 不带 SDL_RENDERER_PRESENTVSYNC：PC 的场节拍由下面的时间轴给，
       交给显示器的刷新率之后，高刷屏上量出来的帧率就不是 60 了。 */
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (s_ren == NULL)
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);   /* 无 3D 驱动 / dummy 驱动 */
    if (s_ren == NULL)
        die("SDL_CreateRenderer");

    /* 缩放用最近邻（sw 第 03 章 3.2 节的 D4）。SDL 对这个 hint 的默认值是**未设**，
       所以必须显式写 "0"；它在建纹理时被读走，之后改没有用。 */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_XRGB8888,
                              SDL_TEXTUREACCESS_STREAMING,
                              (int)FB_WIDTH, (int)FB_HEIGHT);
    if (s_tex == NULL)
        die("SDL_CreateTexture");

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

/* --- 素材分区（sw 第 04 章 3.8 节）----------------------------------------------- */
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

uint32_t board_cycles(void)
{
    uint64_t dt = (uint64_t)SDL_GetPerformanceCounter() - s_perf_base;

#ifdef INJECT_HOST_CYCLES_NAIVE
    /* 注错：直接相乘。本机 SDL 的计数器频率是 1e9，dt * 1e8 在 dt 约 1.845e11
       （184.5 秒）时越过 2^64，之后返回的数值乱跳，不再是任何有意义的周期数。
       见 TechReports sw 第 01 章 4.10，用它做「跑满 200 秒」的对照。 */
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

/* **F1~F3 不在这里**：窗口大小是宿主的事，缩放键在 pump_events 里被吃掉。
   加进来就会进录像、改变回放的按键序列（sw 第 03 章 3.2 节的 D4）。 */
uint32_t board_keys(void)
{
    const Uint8 *k;
    uint32_t     bits = 0u;

    /* 顺手泵一次事件：关窗那一刻要能被看到，哪怕这一帧没有 present。
       （SDL_PumpEvents 只更新键盘状态，SDL_QUIT 要 SDL_PollEvent 才取出来。） */
    pump_events();

    SDL_PumpEvents();
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
    if (k[SDL_SCANCODE_1])                              bits |= BOARD_KEY_MODE1;
    if (k[SDL_SCANCODE_2])                              bits |= BOARD_KEY_MODE2;
    if (k[SDL_SCANCODE_3])                              bits |= BOARD_KEY_MODE3;
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
    uint32_t *fb;

    if (ticket == NULL || index < 0 || index >= BOARD_FB_COUNT || s_present_pending)
        return -1;
    *ticket = s_swaps;
    if (s_quit)
        return 0;

    fb = board_fb(index);
    if (fb != NULL && s_snap_n != 0u)
        snap_maybe(fb, s_swaps);
    if (fb != NULL) {
        SDL_UpdateTexture(s_tex, NULL, fb, (int)FB_STRIDE_BYTES);
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
