/******************************************************************************
* @file main.c: gfx2dGame —— SW1 的最小应用（骨架）
*
* @brief  这个程序不画游戏，它证明三件事：
*         1. 分层能用：应用只看得见 `board.h` 与 `lib/core`，
*            既不认识 sdk 也不认识 SDL，同一份源码在两个平台都编得过；
*         2. 两套构建都对：`make`（板上）与 `make PLATFORM=host`（PC）
*            出来的结果一致；
*         3. 第一条跨平台判据成立：60 帧的整帧 CRC32，
*            板上与 PC 必须逐帧相同。
*
*         每帧向帧缓冲写一个以帧号为种子、由 xorshift32 生成的图案，
*         行跨按 4096 字节算（与板上完全一样），CRC 只覆盖写过的像素，
*         不包括行尾没写过的间隙。
*
*         这里直接写像素只是骨架：游戏正式的画面一律经命令表
*         （SW1-SW9 总览约束 4），由执行器或引擎产生。
*
*         判据见 Engineering/Todo/sw/SW1-软件平台分层与双目标构建.md 第 3 节。
******************************************************************************/

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"
#include "core/xorshift32.h"

/* 帧数。默认 60（PC 上约 1 秒）；做长跑验证时可以覆盖，
   例如 make PLATFORM=host EXTRA_CFLAGS=-DFRAMES=12000 跑约 200 秒，
   用来复查 board_cycles 的换算在长时间运行下是否仍然正确（第 4.10 节）。 */
#ifndef FRAMES
#define FRAMES          60u
#endif

#define ROW_SHIFT       (FB_STRIDE_LOG2 - 2u)   /* 行跨按像素算：4096 字节 = 1024 像素 */
#define ROW_BYTES       (FB_WIDTH * 4u)

/* 以帧号为种子铺满一帧。种子用 frame+1：帧 0 也要有非零种子，
   而 0 是 xorshift 的不动点。 */
static void fill_frame(uint32_t *fb, uint32_t frame)
{
    xs32 g;

    xs32_seed(&g, frame + 1u);
    for (uint32_t y = 0; y < FB_HEIGHT; y++) {
        uint32_t *row = fb + (y << ROW_SHIFT);

        for (uint32_t x = 0; x < FB_WIDTH; x++)
            row[x] = xs32_next(&g);
    }
}

/* 整帧 CRC：逐行喂，因为帧缓冲每行占 4096 字节、像素只占前 2560 字节，
   行尾的间隙没有写过，不能算进去。 */
static uint32_t frame_crc(const uint32_t *fb)
{
    crc32_ctx ctx;

    crc32_begin(&ctx);
    for (uint32_t y = 0; y < FB_HEIGHT; y++)
        crc32_update(&ctx, fb + (y << ROW_SHIFT), ROW_BYTES);
    return crc32_end(&ctx);
}

/* 一行日志：帧号、整帧 CRC、这一步的耗时。
   两个平台打出来的字必须逐字节相同（耗时那一列除外，
   它不是性能结论，见 SW1-SW9 总览约束 5）。 */
static void put_frame_line(uint32_t frame, uint32_t crc, uint32_t cycles)
{
    char     line[80];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "frame ");
    core_out_u32(&o, frame);
    core_out_str(&o, " crc ");
    core_out_hex32(&o, crc);
    core_out_str(&o, " cycles ");
    core_out_u32(&o, cycles);
    core_out_str(&o, "\r\n");
    board_puts(line);
}

int main(void)
{
    board_init();
    crc32_init();
    board_puts("gfx2dGame SW1\r\n");

    for (uint32_t f = 0; f < FRAMES; f++) {
        const int index = (int)(f & 1u);
        uint32_t *fb;
        uint32_t  crc;
        uint32_t  cycles;

        fb = board_fb(index);
        if (fb == NULL) {
            board_puts("board_fb returned NULL\r\n");
            return 1;
        }

        cycles = board_cycles();

        fill_frame(fb, f);

        /* 算 CRC 之前失效一次（E3 规则）：读回的是 DDR 里的字节，
           也就是引擎与扫描输出会看到的那一份，不是缓存里的副本。
           PC 上这个调用是空操作。 */
        board_dcache_invalidate_all();

        crc = frame_crc(fb);
        cycles = board_cycles() - cycles;

        put_frame_line(f, crc, cycles);

        board_present(index);
        board_wait_frame();
    }

    {
        char     line[32];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "frames ");
        core_out_u32(&o, FRAMES);
        core_out_str(&o, "\r\n");
        board_puts(line);
    }
    board_puts("done\r\n");
    return 0;
}
