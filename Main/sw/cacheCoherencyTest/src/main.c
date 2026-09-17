/******************************************************************************
*
* @file main.c: cacheCoherencyTest
*
* @brief  实验 E3：另一个 AXI 主机改写 DDR 之后，CPU 的数据缓存会不会读到旧值。
*
*         引擎是 DDR3 上的第二个 AXI 主机，直接改 DDR。设计文档 2.1 节靠一条
*         「共享区单向使用」的约定绕开一致性问题，代价是命令表写完不能读回校验、
*         素材上传完不能复核、调试时不能用 CPU 读帧缓冲。赛题要求的
*         「软硬件同帧 CRC 对比」恰好要 CPU 读回后缓冲，所以这条约定的现状与
*         代价都要在板上实测。本程序用探针主机 axi_probe_master 当外部写入者。
*
*         完整的实验记录与每一步的依据见
*         Engineering/TechReports/platform/05-CPU数据缓存与第二个主机写入的一致性.md
*
*         ## 读数序列
*
*         每一条判据都按下面的顺序走，arm_line() 做前四步：
*
*           0 读 a ^ 0x2000_0000：行号相同、标签不同，把 a 所在的缓存行换掉。
*             这一步不能省。上一轮留在缓存里的 a（例如 A3 失效之后读回的新值）
*             会遮住第 1 步的别名写，第 2 步于是读到上一轮的值、armed=0；
*           1 别名写（a ^ 0x1000_0000）把 DDR 置成旧值。别名落到同一个颗粒地址，
*             但它的标签与 a 不同，不经过 a 的缓存行；
*           2 读 a：缓存未命中，从 DDR 装进这一行；
*           3 核对读到的确实是旧值，输出里的 armed=，为 0 时这一条判据作废；
*           4 探针主机改写同一地址；
*           5 按组的定义决定是否失效；
*           6 紧跟一次读。
*
*         ## 为什么三种失效方式各占一个函数
*
*         这颗 CPU 上，紧跟在一条「跳走的分支」后面的失效指令，即使没有执行，
*         也会把数据缓存清掉（platform 第 05 章 T10）。如果把三种方式写成同一个函数
*         里的 if，不失效的那条路径就可能被相邻的失效指令误清，判据的结果随编译器
*         怎么排指令而变。拆成三个函数之后，coherency_read_none 里根本没有失效指令，
*         -Og 与 -O0 两种编译档下结果相同（两者都上板实测过）。
*
*         本构型没有写缓冲：CSR 0x810 读写都触发非法指令异常，SoC 网表里是
*         dBus_Bridge_withoutWriteBuffer。所以别名写之后不需要任何排空步骤。
*
*         ## 组
*
*           P 前置      CPU 经别名写、探针读回，确认两个主机看到同一块 DDR
*           A 基线      A1 不失效，预期旧值（缓存遮蔽外部写入）
*                       A2 装行之后访问 64 页别的行号再让探针写，不失效，预期旧值
*                       A3 失效全部，预期新值（与 A1 只差一次失效）
*           B 失效全部  预期新值
*           C 失效单地址 预期新值
*           D 未缓存    探针写之后驱逐该行再读，预期新值（判据自检）
*
*         A 组连跑两遍，第二遍检查反复性，也检查上一遍留下的缓存状态不会串进来。
*
*         ## 注错
*
*           make INJECT=1  去掉 A3、B、C 的失效调用。A3 两行加 B、C 共四行必须报
*                          FAIL (inject)，它们不计入 fail；A1、A2、D、P 不变。
*           make INJECT=2  注错自检：输出按注错模式判定，但失效调用保留。
*                          四行必须报 NOT_RED (inject) 并计入 fail，末尾 fail 4。
*                          这一版证明「注错没生效」不会被当成通过。
*
*         ## 地址
*
*         数据缓存 1 路直接映射、64 行、每行 64 字节，行号取地址 [11:6]、标签取
*         [31:12]（cacheMechTest 的 T4 逐位实测）。每个测试地址给一个不同的行号，
*         测试区在 0x0C80_0000 起，程序、数据与栈都在 0x0000_1000 起的几十 KB 内。
*         栈访问与读字符串也是 DDR 访问，落到被观测行号上就会把它挤掉：
*         读数序列在装行与最后一次读之间不调用打印，只用到栈顶附近的几行；
*         栈顶随产物变化（-Og 版 _sp = 0x3280，-O0 版约 0x4450），换测试地址时要避开。
*
*         寄存器表与数据图样见 rtl/bus/axi_probe_master.v 文件头，基址 0xF810_1000。
*
******************************************************************************/

#include <stdint.h>
#include "bsp.h"
#include "vexriscv.h"

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

#define ST_DONE         0x02u
#define ST_TIMEOUT      0x04u
#define ST_RESPERR      0x08u
#define ST_LASTERR      0x10u

#define MODE_WRITE      0x1u

/* 测试区：一行一个地址，行号取地址 [11:6] */
#define A_BASE          0x0C800000u   /* 行号 0 */
#define A_STRESS        0x0C800040u   /* 行号 1，A2 用的「别的地址」 */
#define B_BASE          0x0C800080u   /* 行号 2 */
#define C_BASE          0x0C8000C0u   /* 行号 3 */
#define D_ARM           0x0C801000u   /* 行号 0，与 A_BASE 同号、标签不同 */
#define P_BASE          0x0C802000u   /* 行号 0，P 组用 */

#define EVICT           0x20000000u   /* 翻转 bit29：行号不变、标签不同 */
#define ALIAS_BIT       0x10000000u   /* 翻转 bit28：DDR 别名，映射同一颗粒地址 */
#define STRESS_PAGE     0x1000u
#define STRESS_N        64u

#define SEED            0x1357A5C0u
#define V_A             0xC0DE00A1u   /* A 组在 DDR 里先放的旧值 */
#define V_B             0xC0DE00B1u
#define V_C             0xC0DE00C1u
#define V_D             0xC0DE00D1u
#define V_P             0xC0DE00F1u
#define V_STRESS        0xC0DE0101u

/* INJECT=1 去掉失效调用；INJECT=2 只按注错模式判定，失效调用保留 */
#if defined(INJECT_NO_INVALIDATE) && !defined(INJECT_REPORT_ONLY)
#define SKIP_INVALIDATE 1
#else
#define SKIP_INVALIDATE 0
#endif

static int fail = 0;

static inline uint32_t rd(uint32_t off)               { return *(volatile uint32_t *)(PRB + off); }
static inline void     wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(PRB + off) = v; }

static void hex8(char *buf, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    for (int k = 7; k >= 0; k--) {
        buf[k] = digits[v & 0xF];
        v >>= 4;
    }
    buf[8] = '\0';
}

/* 探针写到地址 a 的 128 位拍：{v^A5A5A5A5, v^5A5A5A5A, ~v, v}，v = SEED + a。
   AXI 小端，所以 CPU 在 a 读到 v、a+4 读到 ~v、a+8 读到 v^A5A5A5A5、a+12 读到 v^5A5A5A5A */
static inline uint32_t fresh(uint32_t a) { return SEED + a; }

/* 一条判据。expect 一栏在所有编译模式下都打正常模式的期望值，
 * 所以 FAIL 行读起来永远是「期望新值、读到旧值」。
 *
 * inject_fail 标明这一条在注错模式下必须变红（A3、B、C）。注错模式下：
 *   变红    打 FAIL (inject)，不计入 fail；
 *   没变红  打 NOT_RED (inject)，计入 fail。注错没生效时末尾因此不会是 fail 0。 */
struct rec {
    const char *tag;
    const char *sub;
    uint32_t    expect;
    uint32_t    got;
    int         inject_fail;
};

static void report_all(const struct rec *r, int n)
{
    for (int i = 0; i < n; i++) {
        int ok = (r[i].got == r[i].expect);
        int expected_fail = 0;
#ifdef INJECT_NO_INVALIDATE
        expected_fail = r[i].inject_fail;
#endif
        char e[9], g[9];
        hex8(e, r[i].expect);
        hex8(g, r[i].got);
        if (expected_fail) {
            bsp_printf("%s %s expect=%s got=%s %s (inject)\r\n",
                       r[i].tag, r[i].sub, e, g, ok ? "NOT_RED" : "FAIL");
            if (ok)
                fail++;
        } else {
            bsp_printf("%s %s expect=%s got=%s %s\r\n",
                       r[i].tag, r[i].sub, e, g, ok ? "ok" : "FAIL");
            if (!ok)
                fail++;
        }
    }
}

static void report_armed(const char *tag, const char *sub, int armed)
{
    bsp_printf("%s %s armed=%d %s\r\n", tag, sub, armed, armed ? "ok" : "FAIL");
    if (!armed)
        fail++;
}

/* 探针主机：单拍写、单拍读
 *
 * 每一笔之后都要核对错误通路，不能只看 DONE：
 * 硬件超时会同时置 DONE 与 TIMEOUT，轮询 DONE 会正常退出，异常于是被当成成功。
 * 另外 ERRS[15:0] 是非 OKAY 响应计数、STATUS 的 bit4 LAST_ERR 是读突发的 rlast 位置不对，
 * 都要一起查。任何一项异常都计入 fail，避免末尾出现假的 fail 0。 */

static uint32_t probe_write(uint32_t a)
{
    wr32(R_ADDR, a);
    wr32(R_LEN, 0);
    wr32(R_COUNT, 1);
    wr32(R_MODE, MODE_WRITE);
    wr32(R_SEED, SEED);
    wr32(R_CTRL, 1);
    uint32_t st;
    do {
        st = rd(R_STATUS);
    } while (!(st & ST_DONE));
    return st;
}

static uint32_t probe_read(uint32_t a, uint32_t *status)
{
    wr32(R_ADDR, a);
    wr32(R_LEN, 0);
    wr32(R_COUNT, 1);
    wr32(R_MODE, 0);
    wr32(R_SEED, SEED);
    wr32(R_CTRL, 1);
    uint32_t st;
    do {
        st = rd(R_STATUS);
    } while (!(st & ST_DONE));
    (void)rd(R_STATUS);
    (void)rd(R_STATUS);
    uint32_t v = rd(R_RDATA0);
    if (status)
        *status = st;
    return v;
}

/* 核对一笔事务的结果。tag/sub 只用于打印，异常一律计入 fail。
   判据：STATUS 的 bit2 TIMEOUT、bit3 RESP_ERR、bit4 LAST_ERR 都不能置位；
   TX_DONE 必须等于期望的事务数；ERRS 的两个字段都必须为 0 */
static void probe_check(const char *tag, const char *sub, uint32_t st, uint32_t tx_expect)
{
    uint32_t tx  = rd(R_TXDONE);
    uint32_t err = rd(R_ERRS);
    uint32_t bad = 0;
    char a[9];

    if (st & ST_TIMEOUT)  bad |= 1u;
    if (st & ST_RESPERR)  bad |= 2u;
    if (st & ST_LASTERR)  bad |= 4u;
    if (tx != tx_expect)  bad |= 8u;
    if (err != 0)         bad |= 16u;

    if (bad) {
        hex8(a, st);
        bsp_printf("%s %s probe_err status=%s tx=%u/%u errs=", tag, sub, a, tx, tx_expect);
        hex8(a, err);
        bsp_printf("%s code=%u FAIL\r\n", a, bad);
        fail++;
    }
}

/* 在装行与探针写之间制造 DDR 访问压力，全部落在行号 1 的 A_STRESS 那一片，
   不碰被观测的行号 0 */
static void stress_loads(void)
{
    volatile uint32_t acc = 0;
    for (uint32_t k = 0; k < STRESS_N; k++) {
        uint32_t p = A_STRESS + (k + 1u) * STRESS_PAGE;
        *(volatile uint32_t *)p = V_STRESS + k;
        acc += *(volatile uint32_t *)p;
    }
    (void)acc;
}

/* 读数序列第 0~3 步：挤出、别名写、装行、核对。返回 armed。
   这个函数里没有失效指令 */
static int arm_line(uint32_t a, uint32_t stale)
{
    (void)*(volatile uint32_t *)(a ^ EVICT);        /* 0 挤出 a 所在行 */
    *(volatile uint32_t *)(a ^ ALIAS_BIT) = stale;  /* 1 DDR <- stale */
    uint32_t got = *(volatile uint32_t *)a;         /* 2 装行 */
    return got == stale;                            /* 3 */
}

/* 第 4~6 步，不失效。这个函数里没有失效指令 */
static uint32_t coherency_read_none(uint32_t a, uint32_t stale, uint32_t *st_out, int *armed)
{
    *armed = arm_line(a, stale);
    *st_out = probe_write(a);
    return *(volatile uint32_t *)a;
}

/* 第 4~6 步，失效全部 */
static uint32_t coherency_read_all(uint32_t a, uint32_t stale, uint32_t *st_out, int *armed)
{
    *armed = arm_line(a, stale);
    *st_out = probe_write(a);
#if !SKIP_INVALIDATE
    data_cache_invalidate_all();
#endif
    return *(volatile uint32_t *)a;
}

/* 第 4~6 步，失效 a 所在的那一行 */
static uint32_t coherency_read_addr(uint32_t a, uint32_t stale, uint32_t *st_out, int *armed)
{
    *armed = arm_line(a, stale);
    *st_out = probe_write(a);
#if !SKIP_INVALIDATE
    data_cache_invalidate_address(a);
#endif
    return *(volatile uint32_t *)a;
}

/* 各组 */

static void group_pre(void)
{
    uint32_t st = 0;

    /* 让 DDR 里有一个只有 CPU 知道的值，探针读回它。本构型没有写缓冲，
       别名写返回时已经落到 DDR */
    *(volatile uint32_t *)(P_BASE ^ ALIAS_BIT) = V_P;
    uint32_t got = probe_read(P_BASE, &st);
    probe_check("P", "cpu_to_prb", st, 1);

    struct rec r[1] = { { "P", "cpu_to_prb", V_P, got, 0 } };
    report_all(r, 1);
}

static void group_a(void)
{
    struct rec r[3];
    int armed[3];
    uint32_t st = 0;

    /* A1 装行之后紧跟探针写，再读 */
    r[0].tag = "A"; r[0].sub = "immediate";
    r[0].expect = V_A; r[0].inject_fail = 0;
    r[0].got = coherency_read_none(A_BASE, V_A, &st, &armed[0]);
    probe_check("A", "immediate", st, 1);

    /* A2 装行之后访问一批别的行号，再让探针写 */
    armed[1] = arm_line(A_BASE, V_A);
    stress_loads();
    st = probe_write(A_BASE);
    r[1].tag = "A"; r[1].sub = "after_loads";
    r[1].expect = V_A; r[1].inject_fail = 0;
    r[1].got = *(volatile uint32_t *)A_BASE;
    probe_check("A", "after_loads", st, 1);

    /* A3 与 A1 只差一次失效 */
    r[2].tag = "A"; r[2].sub = "invalidated_after";
    r[2].expect = fresh(A_BASE); r[2].inject_fail = 1;
    r[2].got = coherency_read_all(A_BASE, V_A, &st, &armed[2]);
    probe_check("A", "invalidated_after", st, 1);

    report_armed("A", "immediate", armed[0]);
    report_armed("A", "after_loads", armed[1]);
    report_armed("A", "invalidated_after", armed[2]);
    report_all(r, 3);
}

static void group_b(void)
{
    uint32_t st = 0;
    int armed = 0;
    uint32_t got = coherency_read_all(B_BASE, V_B, &st, &armed);
    probe_check("B", "invalidate_all", st, 1);
    struct rec r[1] = { { "B", "invalidate_all", fresh(B_BASE), got, 1 } };
    report_armed("B", "invalidate_all", armed);
    report_all(r, 1);
}

static void group_c(void)
{
    uint32_t st = 0;
    int armed = 0;
    uint32_t got = coherency_read_addr(C_BASE, V_C, &st, &armed);
    probe_check("C", "invalidate_addr", st, 1);
    struct rec r[1] = { { "C", "invalidate_addr", fresh(C_BASE), got, 1 } };
    report_armed("C", "invalidate_addr", armed);
    report_all(r, 1);
}

static void group_d(void)
{
    struct rec r[2];
    uint32_t st = 0;

    /* D1 装行、探针写、驱逐、读，预期新值 */
    (void)arm_line(D_ARM, V_D);
    st = probe_write(D_ARM);
    probe_check("D", "evicted", st, 1);
    (void)*(volatile uint32_t *)(D_ARM ^ EVICT);
    r[0].tag = "D"; r[0].sub = "evicted";
    r[0].expect = fresh(D_ARM); r[0].inject_fail = 0;
    r[0].got = *(volatile uint32_t *)D_ARM;

    /* D2 该 128 位拍的第二个字，同一条未缓存路径 */
    r[1].tag = "D"; r[1].sub = "word1";
    r[1].expect = ~fresh(D_ARM); r[1].inject_fail = 0;
    (void)*(volatile uint32_t *)(D_ARM ^ EVICT);
    r[1].got = *(volatile uint32_t *)(D_ARM + 4);

    report_all(r, 2);
}

static int probe_ident(void)
{
    uint32_t id = rd(R_ID);
    uint32_t arb = rd(R_ARB);
    char a[9], b[9];
    hex8(a, id);
    hex8(b, arb);
    int ok = (id == 0x41584950u) && (arb == 0x6u);
    bsp_printf("IDENT id=0x%s arb=0x%s %s\r\n", a, b, ok ? "ok" : "FAIL");
    return ok;
}

/* 失效代价。打点包住 TIMING_N 次调用，再减去同样次数的空循环。
   单条指令只有几个周期，与读计时器本身的开销同量级，不能逐次打点 */

#define TIMING_N    1000u

static void timing(void)
{
    char a[9], b[9], c[9];
    uint32_t t0, t_all, t_addr, t_read, t_empty;
    volatile uint32_t sink = 0;

    t0 = clint_getTimeLow(BSP_CLINT);
    for (uint32_t k = 0; k < TIMING_N; k++)
        sink = sink + 1u;
    t_empty = clint_getTimeLow(BSP_CLINT) - t0;

    t0 = clint_getTimeLow(BSP_CLINT);
    for (uint32_t k = 0; k < TIMING_N; k++) {
        data_cache_invalidate_all();
        sink = sink + 1u;
    }
    t_all = clint_getTimeLow(BSP_CLINT) - t0;

    t0 = clint_getTimeLow(BSP_CLINT);
    for (uint32_t k = 0; k < TIMING_N; k++) {
        data_cache_invalidate_address(A_BASE);
        sink = sink + 1u;
    }
    t_addr = clint_getTimeLow(BSP_CLINT) - t0;

    t0 = clint_getTimeLow(BSP_CLINT);
    for (uint32_t k = 0; k < TIMING_N; k++) {
        data_cache_invalidate_all();
        sink = sink + *(volatile uint32_t *)A_BASE;
    }
    t_read = clint_getTimeLow(BSP_CLINT) - t0;

    hex8(a, t_empty); hex8(b, t_all); hex8(c, t_addr);
    bsp_printf("TIME raw empty=%s all=%s addr=%s ticks\r\n", a, b, c);

    hex8(a, (t_all  > t_empty) ? (t_all  - t_empty) / TIMING_N : 0u);
    hex8(b, (t_addr > t_empty) ? (t_addr - t_empty) / TIMING_N : 0u);
    hex8(c, t_read);
    bsp_printf("TIME per_call all=%s addr=%s all_plus_read=%s ticks\r\n", a, b, c);
    bsp_printf("TIME n=%d tick=10ns sink=%x\r\n", TIMING_N, sink);
}

void main()
{
    bsp_printf("cacheCoherencyTest\r\n");
#if defined(INJECT_NO_INVALIDATE) && defined(INJECT_REPORT_ONLY)
    bsp_printf("build=INJECT_REPORT_ONLY\r\n");
#elif defined(INJECT_NO_INVALIDATE)
    bsp_printf("build=INJECT_NO_INVALIDATE\r\n");
#else
    bsp_printf("build=normal\r\n");
#endif

    if (!probe_ident()) {
        bsp_printf("probe absent, stop\r\n");
        bsp_printf("fail 1\r\n");
        bsp_printf("done\r\n");
        return;
    }

    group_pre();
    group_a();
    group_a();          /* 第二遍：反复性，以及上一遍的缓存状态不串进来 */
    group_b();
    group_c();
    group_d();
    timing();

    bsp_printf("fail %d\r\n", fail);
    bsp_printf("done\r\n");
}
