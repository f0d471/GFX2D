/******************************************************************************
*
* @file main.c: ddrBurstProbe
*
* @brief  实验 E4：通过 axi_probe_master（DDR3 上的第二个 AXI 主机）测量控制器的
*         突发上限、每笔事务的固定开销与持续吞吐，并做 E2 移交过来的交叉读。
*
*         寄存器表见 rtl/bus/axi_probe_master.v 文件头，基址 0xF810_1000。
*
*         四个阶段，串口逐行输出，任何一步卡死都能从最后一行看出卡在哪：
*
*         1 IDENT   读探针主机 ID 与 ARB 复位值。
*         2 CROSS   交叉读。CPU 写、探针读：CPU 在 A 写 0x5000_0000+k，探针读一拍，RDATA0 应相等；
*                   再读 A+16（CPU 写的是另一个值），RDATA0 应不等，证明判据能分辨。
*                   探针写、CPU 读：探针在 B 写一拍图样，CPU 驱逐缓存后读 B 与 B+4，
*                   应为 v 与 ~v（v = SEED + B）。
*         3 SWEEP   LEN = 0,1,3,...,255，每档固定 4096 拍（65536 字节）：先写、再读比对。
*                   输出事务数、总周期、延迟累计、错误。按 LEN 从小到大，
*                   某一档超出控制器上限而挂死时，最后一行就是那一档。
*         4 SHARE   并发：CPU 做内存拷贝时探针满负载读，对比两侧单独运行与并发运行的耗时。
*
*         测量期间 CPU 只在一个紧循环里轮询 APB 状态寄存器，循环体在指令缓存里，
*         不访问 DDR，尽量不和探针主机争用 Axi_Mux。
*
******************************************************************************/

#include <stdint.h>
#include "bsp.h"
#include "clint.h"

#define PRB             0xF8101000u
#define R_CTRL          0x00
#define R_STATUS        0x04
#define R_ADDR          0x08
#define R_LEN           0x0C
#define R_COUNT         0x10
#define R_MODE          0x14
#define R_SEED          0x18
#define R_TIMEOUT       0x1C
#define R_TXDONE        0x20
#define R_CYCLES        0x24
#define R_LATSUM        0x28
#define R_ERRS          0x2C
#define R_RDATA0        0x30
#define R_RDATA1        0x34
#define R_ARB           0x38
#define R_ID            0x3C

#define ST_BUSY         0x01u
#define ST_DONE         0x02u
#define ST_TIMEOUT      0x04u
#define ST_RESPERR      0x08u
#define ST_LASTERR      0x10u

#define MODE_WRITE      0x1u
#define MODE_INC        0x2u
#define MODE_CHECK      0x4u

#define CROSS_BASE      0x0C900000u
#define SWEEP_BASE      0x0D000000u
#define SHARE_SRC       0x0E000000u
#define SHARE_DST       0x0E800000u
#define EVICT           0x20000000u

static inline uint32_t rd(uint32_t off)            { return *(volatile uint32_t *)(PRB + off); }
static inline void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(PRB + off) = v; }

static void hex8(char *buf, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    for (int k = 7; k >= 0; k--) {
        buf[k] = digits[v & 0xF];
        v >>= 4;
    }
    buf[8] = '\0';
}

static uint32_t now(void)
{
    return clint_getTimeLow(BSP_CLINT);
}

/* 配置并启动一轮，轮询到 DONE，返回 STATUS */
static uint32_t run(uint32_t addr, uint32_t len, uint32_t count, uint32_t mode, uint32_t seed)
{
    wr(R_ADDR, addr);
    wr(R_LEN, len);
    wr(R_COUNT, count);
    wr(R_MODE, mode);
    wr(R_SEED, seed);
    wr(R_CTRL, 1);
    uint32_t st;
    do {
        st = rd(R_STATUS);
    } while (!(st & ST_DONE));
    return st;
}

static uint32_t evict_rd32(uint32_t a)
{
    (void)*(volatile uint32_t *)(a ^ EVICT);
    return *(volatile uint32_t *)a;
}

static int stage_ident(void)
{
    char a[9], b[9];
    uint32_t id = rd(R_ID);
    uint32_t arb = rd(R_ARB);
    hex8(a, id);
    hex8(b, arb);
    int ok = (id == 0x41584950u) && (arb == 0x6u);
    bsp_printf("IDENT id=0x%s arb=0x%s %s\r\n", a, b, ok ? "ok" : "FAIL");
    return !ok;
}

static int stage_cross(void)
{
    int fail = 0;
    char a[9], e[9], g[9];

    /* CPU 写、探针读 */
    for (int k = 0; k < 4; k++) {
        uint32_t A = CROSS_BASE + ((uint32_t)k << 12);
        *(volatile uint32_t *)A = 0x50000000u + (uint32_t)k;
        *(volatile uint32_t *)(A + 16) = 0x60000000u + (uint32_t)k;
    }
    for (int k = 0; k < 4; k++) {
        uint32_t A = CROSS_BASE + ((uint32_t)k << 12);
        uint32_t st = run(A, 0, 1, 0, 0);
        uint32_t got = rd(R_RDATA0);
        int ok = (st == ST_DONE) && (got == 0x50000000u + (uint32_t)k);
        hex8(a, A); hex8(e, 0x50000000u + (uint32_t)k); hex8(g, got);
        bsp_printf("CROSS cpu->prb addr=0x%s expect=0x%s got=0x%s %s\r\n", a, e, g, ok ? "ok" : "FAIL");
        fail += !ok;

        st = run(A + 16, 0, 1, 0, 0);
        got = rd(R_RDATA0);
        ok = (st == ST_DONE) && (got != 0x50000000u + (uint32_t)k);
        hex8(a, A + 16); hex8(g, got);
        bsp_printf("CROSS neighbor addr=0x%s got=0x%s differs %s\r\n", a, g, ok ? "ok" : "FAIL");
        fail += !ok;
    }

    /* 探针写、CPU 读 */
    const uint32_t seed = 0x13570000u;
    for (int k = 0; k < 4; k++) {
        uint32_t B = CROSS_BASE + 0x8000u + ((uint32_t)k << 12);
        uint32_t st = run(B, 0, 1, MODE_WRITE, seed);
        uint32_t v = seed + B;
        uint32_t g0 = evict_rd32(B);
        uint32_t g1 = evict_rd32(B + 4);
        int ok = (st == ST_DONE) && (g0 == v) && (g1 == ~v);
        hex8(a, B); hex8(e, v); hex8(g, g0);
        bsp_printf("CROSS prb->cpu addr=0x%s expect=0x%s got=0x%s %s\r\n", a, e, g, ok ? "ok" : "FAIL");
        fail += !ok;
    }
    return fail;
}

static int stage_sweep(void)
{
    int fail = 0;
    char c[9], l[9], e[9], s[9];

    bsp_printf("SWEEP op len tx cycles latsum errs status\r\n");
    for (int k = 0; k <= 8; k++) {
        uint32_t len = (k == 8) ? 255u : ((1u << k) - 1u);
        uint32_t count = 4096u / (len + 1u);
        uint32_t seed = 0x2468ACE0u + (uint32_t)k;

        for (int op = 0; op < 2; op++) {
            uint32_t mode = MODE_INC | (op == 0 ? MODE_WRITE : MODE_CHECK);
            bsp_printf("SWEEP start %s len=%d\r\n", op == 0 ? "wr" : "rd", len);
            uint32_t st = run(SWEEP_BASE, len, count, mode, seed);
            uint32_t tx = rd(R_TXDONE);
            hex8(c, rd(R_CYCLES));
            hex8(l, rd(R_LATSUM));
            hex8(e, rd(R_ERRS));
            hex8(s, st);
            int ok = (st == ST_DONE) && (tx == count) && (rd(R_ERRS) == 0);
            bsp_printf("SWEEP %s %d %d 0x%s 0x%s 0x%s 0x%s %s\r\n",
                       op == 0 ? "wr" : "rd", len, tx, c, l, e, s, ok ? "ok" : "FAIL");
            fail += !ok;
        }
    }
    return fail;
}

/* CPU 侧负载：逐字拷贝 256KB，数据缓存装不下，每一行都要访问 DDR */
static uint32_t cpu_copy(void)
{
    uint32_t t0 = now();
    for (int rep = 0; rep < 4; rep++) {
        volatile uint32_t *s = (volatile uint32_t *)SHARE_SRC;
        volatile uint32_t *d = (volatile uint32_t *)SHARE_DST;
        for (uint32_t i = 0; i < 65536u; i++)
            d[i] = s[i] + i;
    }
    return now() - t0;
}

static int stage_share(void)
{
    char a[9], b[9], c[9];
    /* 16384 笔 x 256 拍约 50ms，与 CPU 拷贝（约 53ms）重叠；地址固定不递增，避免读到 256MB 以上 */
    const uint32_t len = 255u, count = 16384u;

    /* 探针单独：同一地址读 16384 笔、每笔 256 拍，不比对 */
    uint32_t st = run(SWEEP_BASE, len, count, 0, 0);
    uint32_t prb_alone = rd(R_CYCLES);

    /* CPU 单独 */
    uint32_t cpu_alone = cpu_copy();

    /* 并发：启动探针后立刻做 CPU 拷贝，再等探针结束 */
    wr(R_ADDR, SWEEP_BASE);
    wr(R_LEN, len);
    wr(R_COUNT, count);
    wr(R_MODE, 0);
    wr(R_CTRL, 1);
    uint32_t cpu_shared = cpu_copy();
    uint32_t st2;
    do {
        st2 = rd(R_STATUS);
    } while (!(st2 & ST_DONE));
    uint32_t prb_shared = rd(R_CYCLES);
    int bprb_shared_done = (st2 == ST_DONE);

    hex8(a, prb_alone); hex8(b, prb_shared);
    bsp_printf("SHARE prb cycles alone=0x%s shared=0x%s\r\n", a, b);
    hex8(a, cpu_alone); hex8(b, cpu_shared);
    bsp_printf("SHARE cpu ticks  alone=0x%s shared=0x%s\r\n", a, b);
    hex8(c, rd(R_ERRS));
    bsp_printf("SHARE errs=0x%s %s\r\n", c, (st == ST_DONE && bprb_shared_done) ? "ok" : "FAIL");
    return !(st == ST_DONE && bprb_shared_done);
}

void main()
{
    int fail = 0;
    bsp_printf("ddrBurstProbe\r\n");
    fail += stage_ident();
    fail += stage_cross();
    fail += stage_sweep();
    fail += stage_share();
    bsp_printf("fail %d\r\n", fail);
    bsp_printf("done\r\n");
}
