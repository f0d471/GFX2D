/******************************************************************************
* @file board.c: 板级接口的 PC 实现（SDL2）
*
* @brief  PC 在这套结构里就是「另一块板子」：SDL2 开 640x480 窗口当显示，
*         一块 64MB 的模拟内存当 DDR（分区偏移与板上完全相同），
*         性能计数器折算成 100MHz 的周期数当 CLINT。
*         应用代码里不出现任何平台判断，所以这里必须把板上有的服务都实现出来，
*         哪怕只是空操作。
*
*         本条（SW1）只做最小可用：开窗、显示帧缓冲、60Hz 节拍、按键位图。
*         双缓冲语义（SWAP_REQ / FRAME_DROP）、缩放、录像回放归 SW4。
*
*         帧缓冲行跨固定 4096 字节，上传纹理时按同一个 pitch 交给 SDL，
*         不再另拷贝：PC 与板上的缓冲布局必须一模一样，否则 SW9 的逐帧
*         CRC 对比没有意义。
******************************************************************************/

#define SDL_MAIN_HANDLED        1

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"

static uint8_t  *s_ddr;             /* 模拟的 DDR，64MB */
static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static uint64_t      s_perf_base;
static uint64_t      s_perf_freq;
static uint64_t      s_next_frame;
static int           s_quit;

static void die(const char *what)
{
    fprintf(stderr, "board: %s: %s\n", what, SDL_GetError());
    exit(1);
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

    s_win = SDL_CreateWindow("gfx2d PC (SW1)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             (int)FB_WIDTH, (int)FB_HEIGHT, 0);
    if (s_win == NULL)
        die("SDL_CreateWindow");

    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (s_ren == NULL)
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);   /* 无 3D 驱动的机器 */
    if (s_ren == NULL)
        die("SDL_CreateRenderer");

    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_XRGB8888,
                              SDL_TEXTUREACCESS_STREAMING,
                              (int)FB_WIDTH, (int)FB_HEIGHT);
    if (s_tex == NULL)
        die("SDL_CreateTexture");

    s_perf_freq = (uint64_t)SDL_GetPerformanceFrequency();
    s_perf_base = (uint64_t)SDL_GetPerformanceCounter();
    s_next_frame = s_perf_base;
    s_quit = 0;
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
}

uint32_t board_keys(void)
{
    const Uint8 *k;
    uint32_t     bits = 0u;

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

void board_present(int index)
{
    SDL_Event e;
    uint32_t *fb = board_fb(index);

    if (fb != NULL) {
        SDL_UpdateTexture(s_tex, NULL, fb, (int)FB_STRIDE_BYTES);
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
    }

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            s_quit = 1;
    }
}

void board_wait_frame(void)
{
    uint64_t step = s_perf_freq / 60u;

    if (s_quit || step == 0u)
        return;

    s_next_frame += step;

    for (;;) {
        uint64_t now  = (uint64_t)SDL_GetPerformanceCounter();
        uint64_t left;

        if (now >= s_next_frame)
            break;
        left = s_next_frame - now;
        {
            uint32_t ms = (uint32_t)((left * 1000u) / s_perf_freq);
            SDL_Delay((ms > 0u) ? ms : 1u);
        }
    }

    /* 渲染比一帧还慢时不要攒账：直接对齐到当前时刻，后面按整帧走 */
    if (s_next_frame + step < (uint64_t)SDL_GetPerformanceCounter())
        s_next_frame = (uint64_t)SDL_GetPerformanceCounter();
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
