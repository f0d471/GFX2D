/******************************************************************************
*
* @file main.c: ddrAliasTest
*
* @brief  实验 E2：CPU 看到的 DDR 地址里，哪几位真正参与寻址。
*
*         SoC 把 0x0000_1000~0xE000_0FFF 整段判给 DDR，地址原样送出；
*         顶层只把低 28 位接到 DDR3 控制器。本程序在板上回答三件事：
*
*         1. 低 28 位是否每一位都参与寻址。以 BASE 为基准，逐位翻转第 k 位
*            （k = 0..27）得到 28 个地址，各写一个不同的字节，再逐个读回。
*            某一位没有参与寻址时，翻转它得到的地址与 BASE 落在同一个存储单元，
*            BASE 的值会被覆盖。
*         2. 高 4 位（k = 28..31）是否被丢弃。翻转后写入，读 BASE，
*            预期读到刚写的值，即存在别名、回绕周期 256MB。
*            这一组同时是第 1 组的反例：判据能看到别名，第 1 组读到「无别名」才有意义。
*         3. CPU 数据缓存会不会遮住别名。先读 Q 让它进缓存，再经别名写入，
*            不做驱逐直接读 Q，预期读到缓存里的旧值；驱逐后再读，预期读到新值。
*
*         数据缓存是 1 路直接映射，64 行，每行 64 字节，行号取地址 [11:6]，
*         标签取地址 [31:12]。读一个行号相同、标签不同的地址，会把原来那一行
*         挤出缓存。所以每次校验读之前，先读 X ^ EVICT（第 29 位不同，
*         行号相同），保证随后对 X 的读取从 DDR 重新装载。
*
*         测试区选在 0x0C80_0000 附近，远离程序与栈（0x1000 起不足 1MB）。
*         即使控制器实际只译码 26 或 27 位，这些写入也落不到程序区。
*
******************************************************************************/

#include <stdint.h>
#include "bsp.h"

#define BASE        0x0C800000u
#define EVICT       0x20000000u
#define CACHE_Q     0x0D000000u

static uint8_t rd8(uint32_t a)
{
    return *(volatile uint8_t *)a;
}

static void wr8(uint32_t a, uint8_t v)
{
    *(volatile uint8_t *)a = v;
}

/* 先读同一缓存行、不同标签的地址，把 a 所在行挤出缓存，再读 a */
static uint8_t rd8_fresh(uint32_t a)
{
    (void)rd8(a ^ EVICT);
    return rd8(a);
}

/* bsp_printf 的 %x 不补零，这里固定输出 8 位 */
static void hex8(char *buf, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    for (int k = 7; k >= 0; k--) {
        buf[k] = digits[v & 0xF];
        v >>= 4;
    }
    buf[8] = '\0';
}

static void hex2(char *buf, uint8_t v)
{
    static const char digits[] = "0123456789abcdef";
    buf[0] = digits[v >> 4];
    buf[1] = digits[v & 0xF];
    buf[2] = '\0';
}

static void line(const char *tag, int k, uint32_t addr, uint8_t expect, uint8_t got, int ok)
{
    char a[9], e[3], g[3];
    hex8(a, addr);
    hex2(e, expect);
    hex2(g, got);
    bsp_printf("%s k=%d addr=0x%s expect=%s got=%s %s\r\n",
               tag, k, a, e, g, ok ? "ok" : "FAIL");
}

void main()
{
    const uint8_t base_val = 0x5A;
    int fail = 0;
    char a[9];

    bsp_printf("ddrAliasTest\r\n");

    /* 第 1 组：低 28 位逐位翻转，预期互不别名 */
    wr8(BASE, base_val);
    for (int k = 0; k < 28; k++)
        wr8(BASE ^ (1u << k), (uint8_t)(0x80 + k));

    for (int k = 0; k < 28; k++) {
        uint32_t addr = BASE ^ (1u << k);
        uint8_t got = rd8_fresh(addr);
        int ok = (got == (uint8_t)(0x80 + k));
        line("LOW", k, addr, (uint8_t)(0x80 + k), got, ok);
        fail += !ok;
    }
    {
        uint8_t got = rd8_fresh(BASE);
        int ok = (got == base_val);
        line("LOW", -1, BASE, base_val, got, ok);
        fail += !ok;
    }

    /* 第 2 组：高 4 位逐位翻转，预期写入落到 BASE 上 */
    for (int k = 28; k < 32; k++) {
        uint32_t addr = BASE ^ (1u << k);
        uint8_t v = (uint8_t)(0xC0 + k);
        wr8(addr, v);
        uint8_t got = rd8_fresh(BASE);
        int ok = (got == v);
        line("HIGH", k, addr, v, got, ok);
        fail += !ok;
    }

    /* 第 3 组：缓存遮蔽。预期不驱逐读到旧值 0x11，驱逐后读到新值 0x22 */
    wr8(CACHE_Q, 0x11);
    (void)rd8_fresh(CACHE_Q);                   /* 让 Q 所在行进缓存 */
    wr8(CACHE_Q ^ 0x10000000u, 0x22);            /* 经别名写，缓存里 Q 这一行不更新 */
    {
        uint8_t stale = rd8(CACHE_Q);
        uint8_t fresh = rd8_fresh(CACHE_Q);
        int ok_fresh = (fresh == 0x22);
        /* 不驱逐的那次是观测项，读到哪个值都是结论，不计入 fail */
        line("CACHE_NOEVICT", 0, CACHE_Q, 0x11, stale, stale == 0x11);
        line("CACHE_EVICT", 0, CACHE_Q, 0x22, fresh, ok_fresh);
        fail += !ok_fresh;
    }

    hex8(a, BASE);
    bsp_printf("base 0x%s fail %d\r\n", a, fail);
    bsp_printf("done\r\n");
}
