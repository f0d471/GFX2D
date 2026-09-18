/******************************************************************************
*
* @file main.c: cacheMinTest
*
* @brief  最小实验：这块板上 CPU 的数据缓存到底留不留住一行。
*
*         这个程序写于 E3 判据构造出错的阶段：当时 cacheCoherencyTest 的第一、二版
*         读到了与「缓存会遮蔽外部写入」相反的结果，需要一个不含外部主机的实验
*         分清是缓存不留行、还是判据构造有问题。后来查明是判据构造的问题
*         （platform 第 05 章 4.1、4.4 节）。
*
*         **本程序现在只作为「现象可复现」保留，不作为任何结论的证据。**
*         S3 在别名写之前读过一次 Q ^ EVICT，它自己就可能把 Q 的行换掉，
*         所以 S3 的 FAIL 证明不了「写之后读一次不装行」。这条问题的正确答案
*         由 sw/apps/probe/cacheMechTest 的 T2 给出：只写不装行，写之后读一次会装行。
*
*         本程序把外部主机完全去掉，只问三个互不依赖的问题：
*
*           1 同一地址连读两次，中间夹一次不碰它的 DDR 访问：两次相同吗？
*             （相同说明行留在缓存里；不同说明每次读都走 DDR）
*           2 先写后读：读回来的是刚写的值吗？（写会不会写进缓存）
*           3 CPU 经别名写入之后，原来的地址读到的是旧值还是新值？
*             这一条就是 ddrAliasTest 的 CACHE_NOEVICT，原样复刻，用来
*             确认同一手法在本程序里也成立。
*
*         地址贴近 ddrAliasTest 用的那一片（0x0C80_0000 与 0x0D00_0000），
*         那是已知能观测到缓存遮蔽的地方。测试区在 0x0C80_0000 以上，
*         程序、数据与栈在 0x0000_1000 起的几 KB 内，两端错开。
*
*         输出每项一行 <组> <子项> expect=<8 位 hex> got=<8 位 hex> ok|FAIL，
*         末尾 fail <n> 与 done。
*
******************************************************************************/

#include <stdint.h>
#include "bsp.h"
#include "vexriscv.h"

#define BASE            0x0C800000u   /* 与 ddrAliasTest 的 BASE 相同，缓存行号 0 */
#define BASE_ALT        0x0C800040u   /* 行号 1 */
#define BASE_WR         0x0C800080u   /* 行号 2 */
#define Q               0x0D000000u   /* 与 ddrAliasTest 的 CACHE_Q 相同 */
#define FAR             0x0C900000u   /* 行号 0，标签不同，用来挤同一行号 */

#define EVICT           0x20000000u   /* 翻转 bit29：行号不变、标签不同 */
#define ALIAS           0x10000000u   /* 翻转 bit28：DDR 别名 */
#define DRAM_ALT        0x04000000u   /* 另一片 DDR 区，行号不同，用来制造别的 DDR 访问 */

#define V_A             0xC0DE00A1u
#define V_B             0xC0DE00B2u
#define V_C             0xC0DE00C3u
#define V_Q_OLD         0x00000011u
#define V_Q_NEW         0x00000022u

static int fail = 0;

static void hex8(char *buf, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    for (int k = 7; k >= 0; k--) {
        buf[k] = digits[v & 0xF];
        v >>= 4;
    }
    buf[8] = '\0';
}

static void report(const char *tag, const char *sub, uint32_t expect, uint32_t got)
{
    char e[9], g[9];
    hex8(e, expect);
    hex8(g, got);
    int ok = (got == expect);
    bsp_printf("%s %s expect=%s got=%s %s\r\n", tag, sub, e, g, ok ? "ok" : "FAIL");
    fail += !ok;
}

/* 不透明的空转：不访问 DDR，参数用魔数避免被优化掉 */
static uint32_t spin(uint32_t n)
{
    uint32_t acc = 0;
    for (volatile uint32_t k = 0; k < n; k++)
        acc = acc * 3u + 1u;
    return acc;
}

void main()
{
    char c[9];
    uint32_t sink = 0;
    volatile uint32_t *p;

    bsp_printf("cacheMinTest\r\n");

    /* ------------------------------------------------------------------
     * 1 行留不留：读 A -> 读别处 -> 再读 A
     * ------------------------------------------------------------------ */

    /* 1a 两次读之间只夹寄存器运算，不碰 DDR */
    p = (volatile uint32_t *)BASE;
    *p = V_A;
    uint32_t r1a_1 = *p;
    sink += spin(64);
    uint32_t r1a_2 = *p;
    report("S1", "no_ddr_between", V_A, r1a_2);
    hex8(c, r1a_1);
    bsp_printf("S1 first=%s\r\n", c);

    /* 1b 两次读之间读一片别的 DDR（行号不同），看 A 还在不在 */
    p = (volatile uint32_t *)BASE_ALT;
    *p = V_B;                       /* 让 ALT 也有确定内容 */
    p = (volatile uint32_t *)BASE;
    *p = V_A;
    uint32_t r1b_1 = *p;
    sink += *(volatile uint32_t *)(BASE + DRAM_ALT);   /* 另一片 DDR，行号 0 但标签不同 */
    uint32_t r1b_2 = *p;
    report("S1", "other_ddr_between", V_A, r1b_2);
    hex8(c, r1b_1);
    bsp_printf("S1 first=%s\r\n", c);

    /* 1c 两次读之间读同一行号、别的标签的地址（必然把它挤出），预期读到别处的值 */
    {
        volatile uint32_t *q = (volatile uint32_t *)BASE_WR;
        *q = V_C;
        (void)*q;                                   /* 让 C 那一行进缓存 */
        volatile uint32_t *a = (volatile uint32_t *)BASE;
        *a = V_A;
        uint32_t v1 = *a;
        (void)*(volatile uint32_t *)(BASE ^ EVICT); /* 同一行号、别的标签 */
        uint32_t v2 = *a;
        report("S1", "evicted_same_line", V_A, v2);
        hex8(c, v1);
        bsp_printf("S1 first=%s\r\n", c);
    }

    /* ------------------------------------------------------------------
     * 2 写会不会进缓存：写 -> 读回（不驱逐）
     * ------------------------------------------------------------------ */

    {
        volatile uint32_t *a = (volatile uint32_t *)BASE;
        *a = V_A;
        uint32_t v = *a;
        report("S2", "write_then_read", V_A, v);
    }

    /* 2b 写之后经别名改掉它，原地址不驱逐直接读：ddrAliasTest 的手法 */
    {
        volatile uint32_t *a = (volatile uint32_t *)BASE;
        *a = V_A;
        (void)*a;
        *(volatile uint32_t *)(BASE ^ ALIAS) = V_B;
        uint32_t v = *a;
        report("S2", "alias_no_evict", V_A, v);
        (void)*(volatile uint32_t *)(BASE ^ EVICT);
        uint32_t v2 = *a;
        report("S2", "alias_then_evict", V_B, v2);
    }

    /* ------------------------------------------------------------------
     * 3 原样复刻 ddrAliasTest 的 CACHE_NOEVICT / CACHE_EVICT
     *
     * 注意：这一组的对照**不能**用来判「写之后读一次会不会把行留在缓存里」。
     * 下面的 `(void)*(Q ^ EVICT)` 与 Q 行号相同、标签不同，它本身就可能已经把
     * Q 的那一行换出去了；行被换出之后 `stale = *q` 读到的是别名写之后
     * 从 DDR 取回的新值，与「写有没有装行」无关。
     * 这一项现在只作「现象可复现」保留，不作为任何结论的证据。
     * 要判「装行」用 Main/sw/apps/test/cacheCoherencyTest 的 armed 核对。
     * ------------------------------------------------------------------ */

    {
        volatile uint8_t *q = (volatile uint8_t *)Q;
        *q = (uint8_t)V_Q_OLD;
        (void)*(volatile uint8_t *)(Q ^ EVICT);     /* 装进缓存 */
        *(volatile uint8_t *)(Q ^ ALIAS) = (uint8_t)V_Q_NEW;
        uint8_t stale = *q;                          /* 不驱逐，预期旧值 */
        (void)*(volatile uint8_t *)(Q ^ EVICT);      /* 把 Q 那一行挤出，返回值丢弃 */
        uint8_t fresh = *q;                          /* 紧接着读，预期新值 */
        {
            char g[3];
            static const char digits[] = "0123456789abcdef";
            g[0] = digits[(stale >> 4) & 0xF]; g[1] = digits[stale & 0xF]; g[2] = '\0';
            bsp_printf("S3 CACHE_NOEVICT expect=%02x got=%s %s\r\n",
                       V_Q_OLD, g, (stale == V_Q_OLD) ? "ok" : "FAIL");
            g[0] = digits[(fresh >> 4) & 0xF]; g[1] = digits[fresh & 0xF]; g[2] = '\0';
            bsp_printf("S3 CACHE_EVICT   expect=%02x got=%s %s\r\n",
                       V_Q_NEW, g, (fresh == V_Q_NEW) ? "ok" : "FAIL");
            fail += (fresh != V_Q_NEW);
        }
    }

    hex8(c, sink);
    bsp_printf("sink %s\r\n", c);
    bsp_printf("fail %d\r\n", fail);
    bsp_printf("done\r\n");
}
