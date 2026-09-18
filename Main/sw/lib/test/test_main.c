/******************************************************************************
* @file test_main.c: 主机侧单元测试
*
* @brief  `sw/lib/` 的纯算法测试，不依赖板子、不依赖 SDL。
*         在 64 位与 `-m32` 下各编一份，两边的输出必须逐字节相同：
*         这是 LP64 与 ILP32 差异的第一道闸（第二道是定宽类型与
*         `_Static_assert` 卡住的结构体大小，第三道是板上构建本身）。
*
*         判据见 Engineering/Todo/SW1-软件平台分层与双目标构建.md 第 3 节。
******************************************************************************/

#include <stdio.h>
#include <string.h>

#include "core/crc32.h"
#include "core/format.h"
#include "core/xorshift32.h"
#include "ref_crc.h"

/* 两个平台的区别就在这几行的字面量上：只要有一处类型宽度不对，
   32 位那份测试会先编不过，而不是等到板上才发现。 */
_Static_assert(sizeof(uint32_t) == 4, "uint32_t 必须是 4 字节");
_Static_assert(sizeof(int32_t) == 4, "int32_t 必须是 4 字节");
_Static_assert(sizeof(crc32_ctx) == 4, "crc32_ctx 在两个平台上都必须是 4 字节");
_Static_assert(sizeof(xs32) == 4, "xs32 在两个平台上都必须是 4 字节");

static uint32_t g_fail;

static void out(const char *s)
{
    fputs(s, stdout);
}

static void check(const char *tag, int ok)
{
    char     line[96];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    out(line);

    if (!ok)
        g_fail++;
}

static void check_u32(const char *tag, uint32_t expect, uint32_t got)
{
    char     line[128];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, " expect=");
    core_out_hex32(&o, expect);
    core_out_str(&o, " got=");
    core_out_hex32(&o, got);
    core_out_str(&o, (expect == got) ? " ok\n" : " FAIL\n");
    out(line);

    if (expect != got)
        g_fail++;
}

static void check_indexed(const char *tag, uint32_t index,
                          uint32_t expect, uint32_t got)
{
    char     line[128];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_ch(&o, '[');
    core_out_u32(&o, index);
    core_out_str(&o, "] expect=");
    core_out_hex32(&o, expect);
    core_out_str(&o, " got=");
    core_out_hex32(&o, got);
    core_out_str(&o, (expect == got) ? " ok\n" : " FAIL\n");
    out(line);

    if (expect != got)
        g_fail++;
}

/* --- T1 CRC32 的标准检验值 ------------------------------------------------- */
static void t1_crc_vectors(void)
{
    check_u32("T1 vector empty",     0x00000000u, crc32_ieee("", 0u));
    check_u32("T1 vector a",         0xE8B7BE43u, crc32_ieee("a", 1u));
    check_u32("T1 vector abc",       0x352441C2u, crc32_ieee("abc", 3u));
    check_u32("T1 vector 123456789", 0xCBF43926u, crc32_ieee("123456789", 9u));
}

/* --- T2 查表版与逐位参考实现一致 ------------------------------------------- */
static void t2_crc_vs_ref(void)
{
    static uint8_t buf[4096];
    xs32           g;

    xs32_seed(&g, 0x12345678u);
    for (uint32_t i = 0; i < (uint32_t)sizeof buf; i++)
        buf[i] = (uint8_t)(xs32_next(&g) >> 24);

    check_u32("T2 table vs bitwise", ref_crc32_bitwise(buf, (uint32_t)sizeof buf),
              crc32_ieee(buf, (uint32_t)sizeof buf));
}

/* --- T3 流式与整块一致，且整帧（带行跨）的结果锁在黄金值上 ------------------ */
#define T3_ROWS         8u
#define T3_STRIDE_PX    128u
#define T3_ROW_PX       64u

static void t3_crc_stream(void)
{
    static uint32_t fb[T3_ROWS * T3_STRIDE_PX];
    crc32_ctx       ctx;
    uint32_t        crc;
    xs32            g;

    xs32_seed(&g, 1u);
    for (uint32_t y = 0; y < T3_ROWS; y++)
        for (uint32_t x = 0; x < T3_ROW_PX; x++)
            fb[y * T3_STRIDE_PX + x] = xs32_next(&g);

    /* 逐行喂：帧缓冲行跨 4096 字节，行内只有前一半像素写过，
       所以只能按行 update，不能把整块（含没写过的间隙）一次算进去。 */
    crc32_begin(&ctx);
    for (uint32_t y = 0; y < T3_ROWS; y++)
        crc32_update(&ctx, &fb[y * T3_STRIDE_PX], T3_ROW_PX * 4u);
    crc = crc32_end(&ctx);

    check_u32("T3 strided golden", 0x61A39581u, crc);
    check_u32("T3 strided vs ref", ref_crc32_strided((const uint8_t *)fb, T3_ROWS,
                                                     T3_STRIDE_PX * 4u, T3_ROW_PX * 4u),
              crc);

    /* 同样的行按另一组分段喂（3 行 + 3 行 + 2 行），结果必须一样：
       增量接口的分段方式不能影响结果。 */
    crc32_begin(&ctx);
    for (uint32_t y = 0; y < 3u; y++)
        crc32_update(&ctx, &fb[y * T3_STRIDE_PX], T3_ROW_PX * 4u);
    for (uint32_t y = 3u; y < 6u; y++)
        crc32_update(&ctx, &fb[y * T3_STRIDE_PX], T3_ROW_PX * 4u);
    for (uint32_t y = 6u; y < T3_ROWS; y++)
        crc32_update(&ctx, &fb[y * T3_STRIDE_PX], T3_ROW_PX * 4u);
    check_u32("T3 split equals joined", crc, crc32_end(&ctx));
}

/* --- T4 xorshift32 --------------------------------------------------------- */
static const uint32_t T4_SEED1[8] = {
    0x00042021u, 0x04080601u, 0x9DCCA8C5u, 0x1255994Fu,
    0x8EF917D1u, 0x2C6F5BD0u, 0x25B2331Au, 0x19F91CB2u
};
static const uint32_t T4_SEED7[8] = {
    0x001CE0E7u, 0x1C099207u, 0xE765C143u, 0xB6FC5245u,
    0xAA296FFFu, 0x7D0F4E94u, 0x0F6D1BBAu, 0x2F5BBB57u
};

static void t4_xorshift(void)
{
    xs32 g1, g7, g0, gw;

    xs32_seed(&g1, 1u);
    xs32_seed(&g7, 7u);
    for (uint32_t i = 0; i < 8u; i++) {
        check_indexed("T4 seed1", i, T4_SEED1[i], xs32_next(&g1));
        check_indexed("T4 seed7", i, T4_SEED7[i], xs32_next(&g7));
    }

    /* 种子 0 是不动点，必须被换成 1，否则随机数会卡死 */
    xs32_seed(&g0, 0u);
    xs32_seed(&gw, 1u);
    check_u32("T4 seed0 maps to 1", xs32_next(&gw), xs32_next(&g0));
}

/* --- T5 轻量格式化输出 ----------------------------------------------------- */
static void t5_format(void)
{
    char     line[16];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "f");
    core_out_ch(&o, ' ');
    core_out_u32(&o, 60u);
    core_out_ch(&o, ' ');
    core_out_hex32(&o, 0x1A2Bu);
    core_out_str(&o, " %");
    check("T5 plain", strcmp(line, "f 60 00001a2b %") == 0);

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_u32_pad(&o, 7u, 4u);
    check("T5 width", strcmp(line, "   7") == 0);

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_u32(&o, 4294967295u);
    check("T5 u32 max", strcmp(line, "4294967295") == 0);

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_hex32(&o, 0u);
    check("T5 hex32 zero", strcmp(line, "00000000") == 0);

    /* 截断：长度照算，缓冲区里留下能放下的那几个字符 */
    core_out_init(&o, line, 5u);
    core_out_str(&o, "abcdefgh");
    check("T5 truncate", (core_out_len(&o) == 8u) && (strcmp(line, "abcd") == 0));

    core_out_init(&o, line, 4u);
    core_out_u32(&o, 123456789u);
    check("T5 truncate digits", (core_out_len(&o) == 9u) && (strcmp(line, "123") == 0));

    /* cap 为 0：一个字都不写，也不越界 */
    core_out_init(&o, NULL, 0u);
    core_out_str(&o, "abc");
    check("T5 cap0", core_out_len(&o) == 3u);
}

int main(void)
{
    out("libTest crc32/xorshift32/format\n");

    /* 独立参考实现与查表实现的结果先对齐，后面的黄金值才有意义 */
    t1_crc_vectors();
    t2_crc_vs_ref();
    t3_crc_stream();
    t4_xorshift();
    t5_format();

    {
        char     line[48];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "fail ");
        core_out_u32(&o, g_fail);
        core_out_str(&o, "\n");
        out(line);
    }
    out("done\n");
    return (g_fail == 0u) ? 0 : 1;
}
