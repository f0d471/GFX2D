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

/* guard（rtl/bus/axi_mux_guard.v）与扫描替身（rtl/bus/scanout_stub.v）窗口内的偏移 */
#define GRD             0xF810F000u
#define SCN             0xF810E000u
#define G_CTRL          0x08
#define G_MASK          0x0C
#define G_AGE           0x10
#define G_STATUS        0x14
#define G_HITS          0x18
#define G_MAX_AR        0x1C
#define G_MAX_AW        0x20
#define G_CLEAR         0x24
#define S_CTRL          0x08
#define S_BASE          0x0C
#define S_MAX_FIRST     0x18
#define S_MAX_FULL      0x1C
#define S_MAX_GAP       0x20
#define S_TXN           0x24
#define S_BEAT          0x2C
#define S_SKIP          0x30
#define S_RESPERR       0x34
#define S_STATUS        0x38
#define FRAMEBUF0       0x02000000u

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

static inline uint32_t grd(uint32_t off)            { return *(volatile uint32_t *)(GRD + off); }
static inline void     grdw(uint32_t off, uint32_t v){ *(volatile uint32_t *)(GRD + off) = v; }
static inline uint32_t scn(uint32_t off)            { return *(volatile uint32_t *)(SCN + off); }
static inline void     scnw(uint32_t off, uint32_t v){ *(volatile uint32_t *)(SCN + off) = v; }

/* 1 tick = 10 ns（core_clk 100MHz） */
#define TICKS_PER_MS    100000u
static void wait_ms(uint32_t ms)
{
    uint32_t t0 = now();
    while ((now() - t0) < ms * TICKS_PER_MS) { }
}

static int stage_ident(void)
{
    char a[9], b[9], c[9];
    uint32_t id = rd(R_ID);
    uint32_t arb = rd(R_ARB);
    hex8(a, id);
    hex8(b, arb);
    int ok = (id == 0x41584950u) && (arb == 0x6u);
    bsp_printf("IDENT id=0x%s arb=0x%s %s\r\n", a, b, ok ? "ok" : "FAIL");
    int fail = !ok;

    /* E5 配置归属：guard 与扫描替身的窗口与上电默认值 */
    uint32_t gc = grd(G_CTRL), gm = grd(G_MASK), ga = grd(G_AGE);
    int gok = (grd(0x00) == 0x4D555847u) && (grd(0x04) == 1u) &&
              (gc == 3u) && (gm == 7u) && (ga == 64u);
    bsp_printf("IDENT guard ctrl=%d mask=%d age=%d %s\r\n", gc, gm, ga, gok ? "ok" : "FAIL");
    fail += !gok;

    hex8(c, scn(0x00));
    int sok = (scn(0x00) == 0x5343414Eu) && (scn(0x04) == 1u) && (scn(S_BASE) == FRAMEBUF0);
    bsp_printf("IDENT scan id=0x%s base=0x%s %s\r\n", c, (scn(S_BASE) == FRAMEBUF0) ? "02000000" : "other", sok ? "ok" : "FAIL");
    fail += !sok;
    return fail;
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

/* E5：三方并发。扫描替身按行读 160 拍、探针替身读满负载、CPU 拷贝三者同时跑，
   记录扫描的首拍/整笔/最大拍间隙、guard 的 SoC 地址等待与命中数、CPU 拷贝耗时。
   板上只报告黑盒控制器下的实测最大值，不声称形式上界（见 E5 2.2 节）。 */
static int stage_tri(void)
{
    char a[9], b[9], c[9], d[9];
    const uint32_t len = 255u, count = 16384u;
    int fail = 0;

    scnw(S_BASE, FRAMEBUF0);
    scnw(S_CTRL, 0);
    scnw(S_CTRL, 1);            /* 使能扫描替身 */
    grdw(G_CLEAR, 1);           /* 清 guard 统计 */

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

    uint32_t hits = grd(G_HITS), war = grd(G_MAX_AR), waw = grd(G_MAX_AW);
    uint32_t txn = scn(S_TXN), skip = scn(S_SKIP), rerr = scn(S_RESPERR);
    uint32_t f = scn(S_MAX_FIRST), fu = scn(S_MAX_FULL), gp = scn(S_MAX_GAP);

    hex8(a, cpu_shared);
    bsp_printf("TRI cpu ticks=0x%s\r\n", a);
    hex8(a, war); hex8(b, waw); hex8(c, hits); hex8(d, grd(G_STATUS));
    bsp_printf("TRI guard max_wait_ar=0x%s max_wait_aw=0x%s hits=0x%s status=0x%s\r\n", a, b, c, d);
    hex8(d, st2);
    bsp_printf("TRI prb status=0x%s\r\n", d);
    bsp_printf("TRI scan txn=%d beat=%d skip=%d resperr=%d\r\n", txn, scn(S_BEAT), skip, rerr);
    bsp_printf("TRI scan max_first=%d max_full=%d max_gap=%d\r\n", f, fu, gp);

    /* 判据：三方都完成，扫描没被跳过、没有响应错误 */
    int ok = (st2 == ST_DONE) && (txn > 0) && (skip == 0) && (rerr == 0);
    bsp_printf("TRI %s\r\n", ok ? "ok" : "FAIL");
    fail += !ok;
    return fail;
}

/* E5：保护的上板判据。AGE_LIMIT=1 时 GUARD_HITS 必须增加且三方照常完成；
   再把 S0 的掩码位清掉当反例，扫描替身必须完全拿不到授权。 */
static int stage_inject(void)
{
    char a[9];
    const uint32_t len = 255u, count = 4096u;
    int fail = 0;

    /* 1) AGE_LIMIT=1 */
    grdw(G_AGE, 1u);
    grdw(G_CLEAR, 1u);
    scnw(S_CTRL, 1);
    wr(R_ADDR, SWEEP_BASE);
    wr(R_LEN, len);
    wr(R_COUNT, count);
    wr(R_MODE, 0);
    wr(R_CTRL, 1);
    (void)cpu_copy();
    uint32_t st2;
    do {
        st2 = rd(R_STATUS);
    } while (!(st2 & ST_DONE));
    uint32_t h1 = grd(G_HITS);
    hex8(a, h1);
    int ok1 = (h1 > 0u) && (st2 == ST_DONE);
    bsp_printf("INJECT age1 hits=0x%s %s\r\n", a, ok1 ? "ok" : "FAIL");
    fail += !ok1;

    /* 设回 64，同样负载，只记录命中数 */
    grdw(G_AGE, 64u);
    grdw(G_CLEAR, 1u);
    wr(R_ADDR, SWEEP_BASE);
    wr(R_LEN, len);
    wr(R_COUNT, count);
    wr(R_MODE, 0);
    wr(R_CTRL, 1);
    (void)cpu_copy();
    do {
        st2 = rd(R_STATUS);
    } while (!(st2 & ST_DONE));
    hex8(a, grd(G_HITS));
    bsp_printf("INJECT age64 hits=0x%s (record only)\r\n", a);

    /* 2) S0 掩码反例 */
    grdw(G_MASK, 7u);
    scnw(S_CTRL, 1);
    uint32_t t0 = scn(S_TXN);
    wait_ms(20);
    uint32_t t1 = scn(S_TXN);
    grdw(G_MASK, 6u);           /* 清掉 S0 位：反例 */
    wait_ms(20);
    uint32_t t2 = scn(S_TXN);
    wait_ms(20);
    uint32_t t3 = scn(S_TXN);
    grdw(G_MASK, 7u);           /* 恢复 */
    wait_ms(20);
    uint32_t t4 = scn(S_TXN);
    int ok2 = (t1 > t0) && (t2 == t1) && (t3 == t1) && (t4 > t3);
    bsp_printf("INJECT mask_s0 txn %d %d %d %d %d %s\r\n", t0, t1, t2, t3, t4, ok2 ? "ok" : "FAIL");
    fail += !ok2;

    scnw(S_CTRL, 0);            /* 关掉扫描替身，别影响后面的回归 */
    return fail;
}

void main()
{
    int fail = 0;
    bsp_printf("ddrBurstProbe\r\n");
    fail += stage_ident();
    fail += stage_cross();
    fail += stage_sweep();
    fail += stage_share();
    fail += stage_tri();
    fail += stage_inject();
    bsp_printf("fail %d\r\n", fail);
    bsp_printf("done\r\n");
}
