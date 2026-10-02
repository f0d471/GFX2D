/******************************************************************************
*
* @file main.c: cacheCoherencyTest —— E3 回归的引擎改写版（B6）
*
* @brief  B6 接入引擎时探针主机撤下（引擎 P1 的 D6/D22），本程序的外部写入者
*         从 axi_probe_master 换成引擎（FILL COPY），第二个读者换成引擎 BLIT
*         （读 CPU 写过的表面再拷一份，CPU 失效后读回）。E3 的机理实验与
*         「为什么」见 platform 第 05 章；探针版的最后一份基线日志在
*         logs/b6_preintegration_20261002.log。判据不变：引擎写入后失效读到
*         新值、不失效读到旧值。
*
*         ## 组
*
*           P 前置      引擎在位；引擎 FILL 写入、CPU 失效后读到新值
*           A 基线      A1 引擎 FILL 之后不失效，预期旧值（缓存遮蔽外部写入）
*                       A2 装行之后再访问 64 个同行号、不同标签的地址（被观测
*                          行不受影响），引擎写，不失效，预期旧值
*                       A3 失效全部，预期新值（与 A1 只差一次失效）
*           B 失效全部  预期新值
*           C 失效单地址 预期新值
*           D 第二读者  CPU 经缓存写源表面，引擎 BLIT 拷到另一面，CPU 失效后
*                       读回，预期与源逐字相同
*
*         ## 读数序列与栈纪律
*
*         每条判据：CPU 写旧值并读一次（装行）→ 引擎 FILL 新值 → 按组定义失效
*         或不失效 → 读一次。装行与最后一次读之间不调用打印（board_puts 也是
*         DDR 访问，落到被观测行号上会把它挤掉），只经栈顶附近留几个局部量。
*         数据缓存 1 路直接映射、64 行、每行 64 字节，行号取地址 [11:6]
*         （cacheMechTest 的 T4 逐位实测，归档在 platform 第 05 章）。
*
*         ## 为什么三种失效方式各占一个函数
*
*         这颗 CPU 上，紧跟在一条「跳走的分支」后面的失效指令，即使没有执行，
*         也会把数据缓存清掉（platform 第 05 章 T10）。不失效的路径必须住在
*         一个根本没有失效指令的函数里，判据结果才不随编译器排指令而变
*         （探针版在 -Og 与 -O0 两档都实测过；本版沿用同一结构）。
*
*         ## 注错
*
*           make INJECT=1  去掉 A3、B、C 的失效调用。这四行必须报 FAIL (inject)
*                          且不计入 fail（它们读旧值正是注错想要的效果）；
*                          A1、A2、D、P 不变。末尾 fail 0。
*           make INJECT=2  注错自检：失效调用保留，但判定按注错口径走。
*                          四行必须报 NOT_RED (inject) 并计入 fail，末尾 fail 4。
*                          这一版证明「注错没生效」不会被当成通过。
*
*         ## 地址
*
*         测试区在共享池前段（REGION_SHARED，0x0064_0000 起），每个测试地址
*         行号不同；栈在程序区 0x0000_1000 起的几十 KB 内，与测试地址不同行号。
*         引擎写的表面是 64×1 的 XRGB（FILL COPY 一条命令写 64 个字），
*         命令表放 REGION_CMDLIST_A（基址 32 字节对齐，D53）。
*
*         引擎访问一律走 sw/bsp 的 gfx_engine 驱动，本文件不出现寄存器偏移。
******************************************************************************/
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "gfx_engine.h"
#include "gfx/gfx.h"

/* --- 测试地址（行号取 [11:6]，互不相同） ---------------------------------- */
#define A_BASE      0x00640400u     /* 行号 0：A 组的被观测字 */
#define A_STRESS    0x00640440u     /* 行号 1：A2 用来访问 64 个别的标签 */
#define B_BASE      0x00640480u     /* 行号 2 */
#define C_BASE      0x006404C0u     /* 行号 3 */
#define D_SRC       0x00640600u     /* D 组 CPU 写的源（64×1） */
#define D_DST       0x00640800u     /* D 组引擎 BLIT 的目的（64×1） */
#define P_BASE      0x00640C00u     /* P 组 */

#define SURF_WORDS  64u
#define CMDLIST     0x00400000u
#define V_NEW       0x00C0DEF1u     /* 引擎 FILL 的新值 */

#define STRESS_N    64u

static int fails = 0;

static void ok_line(const char *what, const char *verdict)
{
    board_puts(what);
    board_puts(" ");
    board_puts(verdict);
    board_puts("\n");
}

/* --- 注错与判定 ------------------------------------------------------------ */

#ifdef INJECT_2
#define REPORT_ONLY 1
#else
#define REPORT_ONLY 0
#endif
#ifdef INJECT_1
#define SKIP_INV 1
#else
#define SKIP_INV 0
#endif

/* 每行带 got=<8 位十六进制>：失败时分辨「读回旧值」（缓存/转发问题）与
   「读回别的」（地址/译码问题）。 */
static void put_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    char s[14];
    int i;

    s[0] = ' ';
    s[1] = 'g';
    s[2] = 'o';
    s[3] = 't';
    s[4] = '=';
    for (i = 0; i < 8; i++)
        s[5 + i] = hex[(v >> (28 - 4 * i)) & 0xFu];
    s[13] = '\0';
    board_puts(s);
}

/* expect_new=1 的组：读到新值才算 ok。injectable=1 的组（A3、B、C）在三个
   构建里分岔；其余组（P、D、A1、A2）三个构建同一口径。 */
static void judge(const char *what, uint32_t got, uint32_t expect_new,
                  uint32_t injectable)
{
    put_hex(got);
    if (!injectable) {
        ok_line(what, (got == V_NEW) == (expect_new != 0u) ? "ok" : "FAIL");
        if ((got == V_NEW) != (expect_new != 0u))
            fails++;
        return;
    }
    if (got == V_NEW) {
        if (!SKIP_INV && !REPORT_ONLY)
            ok_line(what, "ok");
        else if (REPORT_ONLY) {
            /* INJECT_2：失效保留、读回新值，按注错口径判定必须红而没红 */
            ok_line(what, "NOT_RED (inject)");
            fails++;
        } else {
            /* SKIP_INV 下读到新值：注错没生效，判据必须见红 */
            ok_line(what, "FAIL");
            fails++;
        }
    } else {
        if (!SKIP_INV && !REPORT_ONLY) {
            ok_line(what, "FAIL");
            fails++;
        } else if (REPORT_ONLY) {
            ok_line(what, "NOT_RED (inject)");
            fails++;
        } else {
            /* SKIP_INV（INJECT_1）：读旧值正是注错的效果，这行算注错见红 */
            ok_line(what, "FAIL (inject)");
        }
    }
}

/* --- 引擎侧 ---------------------------------------------------------------- */

/* 把表面表里 valid 的槽登记进引擎 */
static int engine_load(const gfx_surface_table *t)
{
    int s;

    for (s = 0; s < (int)GFX_SURF_N; s++) {
        uint32_t ctrl = t->s[s].ctrl;
        if ((ctrl & 0x80000000u) == 0u)
            continue;
        if (gfx_engine_surf_set(s, t->s[s].base,
                                t->s[s].dim & 0xFFFFu, t->s[s].dim >> 16,
                                ctrl & 0x1Fu, (ctrl >> 12) & 0xFu) != GFX_ENG_OK)
            return -1;
    }
    return 0;
}

static uint32_t engine_run(const gfx_surface_table *t,
                           const gfx_cmd *cmds, uint32_t n)
{
    uint32_t code = 0u, eidx, etag;

    if (engine_load(t) != 0)
        return 0xFFu;
    memcpy((void *)(uintptr_t)CMDLIST, cmds, (size_t)n * 32u);
    if (gfx_engine_submit(CMDLIST, n) != GFX_ENG_OK)
        return 0xFFu;
    if (gfx_engine_wait(&code, &eidx, &etag) != GFX_ENG_OK || code != 0u)
        return (code == 0u) ? 0xFFu : code;
    return 0u;
}

static uint32_t engine_fill(uint32_t base, uint32_t color)
{
    static gfx_surface_table tbl;
    static gfx_cmd cmds[2];
    gfx_list l;
    gfx_surface s;
    gfx_rect r;

    gfx_surface_reset(&tbl);
    s.base = base; s.width = SURF_WORDS; s.height = 1u;
    s.stride_log2 = 8u; s.space = GFX_SPACE_DDR; s.format = GFX_FMT_XRGB8888;
    gfx_surface_set(&tbl, 0, &s);
    gfx_list_begin(&l, cmds, 2u, &tbl);
    r.x0 = 0; r.y0 = 0; r.x1 = (int)SURF_WORDS - 1; r.y1 = 0;
    gfx_fill(&l, 0, r, color, GFX_EQ_COPY, 0u);
    gfx_list_end(&l);
    return engine_run(&tbl, cmds, l.used);
}

/* --- CPU 侧（失效方式各占一个函数，见文件头） ------------------------------ */

static uint32_t read_none(const volatile uint32_t *p)
{
    return *p;
}

/* 失效方式各占一个函数；INJECT_1 的「去掉失效」也收在函数内部，
   调用点三个构建完全一致——判据的差别只剩 judge() 一处。 */
static uint32_t read_inv_all(const volatile uint32_t *p)
{
    uint32_t v;

#if !SKIP_INV
    board_dcache_invalidate_all();
#endif
    v = *p;
    return v;
}

static uint32_t read_inv_addr(const volatile uint32_t *p)
{
    uint32_t v;

#if !SKIP_INV
    board_dcache_invalidate_range((const void *)(uintptr_t)p, 4u);
#endif
    v = *p;
    return v;
}

/* CPU 把旧值写进 DDR 并读一次（装行）。返回读到的值，恒等于旧值。
   三步与探针版「读数序列」第 0~2 步等价：
   0 读 a^0x2000_0000（行号相同、标签不同）把被观测行换掉——上一轮 A3 失效后
     读回的新值还留在这一行里，不换掉它会遮住别名写，装进缓存的就是新值
     （本轮实测：省掉这一步时第二遍的 A1/A2 全部读回上一轮的新值）；
   1 经**别名地址**（翻转 bit28，同一颗粒地址、不同缓存标签，E2 实测）把 DDR
     置成旧值。被观测行不能用 CPU 直写（直写后失效行为与探针基线对不上）；
   2 读被观测地址：缓存未命中，从 DDR 装进这一行（旧值）。 */
static uint32_t arm_line(uint32_t addr, uint32_t old)
{
    uint32_t evict;

    evict = *(volatile uint32_t *)(uintptr_t)(addr ^ 0x20000000u);
    *(volatile uint32_t *)(uintptr_t)(addr ^ 0x10000000u) = old | (evict & 0u);
    return *(volatile uint32_t *)(uintptr_t)addr;
}

/* A2：访问 64 个同行号、不同标签的地址（步长 0x1000 翻标签、不动行号），
   被观测行不受影响。访问结果折进一个字写到别的行号上，防被优化掉。 */
static void poke_lines(void)
{
    const volatile uint32_t *p =
        (const volatile uint32_t *)(uintptr_t)A_STRESS;
    uint32_t acc = 0u;
    uint32_t i;

    for (i = 0; i < STRESS_N; i++)
        acc ^= p[i * (0x1000u / 4u)];
    *(volatile uint32_t *)(uintptr_t)(A_STRESS + 0x2000u) = acc;
}

/* --- 组 -------------------------------------------------------------------- */

static void group_a(int pass, uint32_t old)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)A_BASE;
    char name[4];
    uint32_t got;

    name[0] = 'A'; name[1] = (char)('1'); name[2] = '\0';
    (void)pass;

    /* A1：装行 → 引擎写 → 不失效 → 读旧值 */
    arm_line(A_BASE, old);
    if (engine_fill(A_BASE, V_NEW) != 0u) {
        ok_line(name, "FAIL");
        fails++;
        return;
    }
    got = read_none(p);
    judge(name, got, 0u, 0u);

    /* A2：先访问 64 个别的标签再走一遍同一序列 */
    name[1] = '2';
    poke_lines();
    arm_line(A_BASE, old);
    if (engine_fill(A_BASE, V_NEW) != 0u) {
        ok_line(name, "FAIL");
        fails++;
        return;
    }
    got = read_none(p);
    judge(name, got, 0u, 0u);

    /* A3：与 A1 只差一次失效全部 */
    name[1] = '3';
    arm_line(A_BASE, old);
    if (engine_fill(A_BASE, V_NEW) != 0u) {
        ok_line(name, "FAIL");
        fails++;
        return;
    }
    got = read_inv_all(p);
    judge(name, got, 1u, 1u);
}

static void group_b(uint32_t old)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)B_BASE;
    uint32_t got;

    arm_line(B_BASE, old);
    if (engine_fill(B_BASE, V_NEW) != 0u) {
        ok_line("B", "FAIL");
        fails++;
        return;
    }
    got = read_inv_all(p);
    judge("B", got, 1u, 1u);
}

static void group_c(uint32_t old)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)C_BASE;
    uint32_t got;

    arm_line(C_BASE, old);
    if (engine_fill(C_BASE, V_NEW) != 0u) {
        ok_line("C", "FAIL");
        fails++;
        return;
    }
    got = read_inv_addr(p);
    judge("C", got, 1u, 1u);
}

static void group_d(void)
{
    static gfx_surface_table tbl;
    static gfx_cmd cmds[2];
    uint32_t *src = (uint32_t *)(uintptr_t)D_SRC;
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)D_DST;
    gfx_list l;
    gfx_surface s;
    uint32_t i, bad = 0u;

    /* CPU 经缓存写源表面（写穿到 DDR），引擎 BLIT 拷到目的面，
       CPU 失效后读回必须逐字相同 */
    for (i = 0; i < SURF_WORDS; i++)
        src[i] = 0x00570000u + i;

    gfx_surface_reset(&tbl);
    s.base = D_SRC; s.width = SURF_WORDS; s.height = 1u;
    s.stride_log2 = 8u; s.space = GFX_SPACE_DDR; s.format = GFX_FMT_XRGB8888;
    gfx_surface_set(&tbl, 0, &s);
    s.base = D_DST;
    gfx_surface_set(&tbl, 1, &s);
    gfx_list_begin(&l, cmds, 2u, &tbl);
    gfx_blit(&l, 0, 0, 0, 1, 0, 0, (int)SURF_WORDS, 1, GFX_EQ_COPY, 0u, 0, 0u);
    gfx_list_end(&l);
    if (engine_run(&tbl, cmds, l.used) != 0u) {
        ok_line("D", "FAIL");
        fails++;
        return;
    }
    board_dcache_invalidate_range((const void *)(uintptr_t)dst, SURF_WORDS * 4u);
    for (i = 0; i < SURF_WORDS; i++)
        if (dst[i] != 0x00570000u + i)
            bad++;
    ok_line("D", bad == 0u ? "ok" : "FAIL");
    if (bad != 0u)
        fails++;
}

/* --- 入口 ------------------------------------------------------------------- */

int main(void)
{
    uint32_t got;

    board_init();
    /* 本程序只跑几百毫秒。实测停两秒仍会在 board_run 的 OpenOCD 会话退出前
       打完 done，整段输出落在加载分界之前，--stop-on 因而误报未完成。
       启动后等八秒，使本次程序的判据落在分界之后；不改变缓存实验的读数序列。 */
    while (board_cycles() < 8u * BOARD_HZ)
        ;
    board_puts("cacheCoherencyTest B6\n");

    if (!gfx_engine_present() || gfx_engine_soft_reset() != GFX_ENG_OK) {
        ok_line("IDENT", "FAIL");
        board_puts("fail 1\n");
        board_puts("done\n");
        return 0;
    }
    ok_line("IDENT", "ok");

    /* P 前置：引擎 FILL 写入、CPU 失效后读到新值（引擎通路本身的可用性） */
    if (engine_fill(P_BASE, V_NEW) == 0u) {
        got = read_inv_all((volatile uint32_t *)(uintptr_t)P_BASE);
        judge("P", got, 1u, 0u);
    } else {
        ok_line("P", "FAIL");
        fails++;
    }

    group_a(0, 0xC0DE00A1u);
    group_a(1, 0xC0DE00A2u);    /* 第二遍：反复性，旧值换个数 */
    group_b(0xC0DE00B1u);
    group_c(0xC0DE00C1u);
    group_d();

    gfx_engine_soft_reset();
    {
        char line[16];
        char *p = line;
        int n = fails;
        *p++ = 'f'; *p++ = 'a'; *p++ = 'i'; *p++ = 'l'; *p++ = ' ';
        if (n == 0) *p++ = '0';
        else {
            char buf[8];
            int m = 0;
            while (n > 0) { buf[m++] = (char)('0' + n % 10); n /= 10; }
            while (m > 0) *p++ = buf[--m];
        }
        *p++ = '\n'; *p = '\0';
        board_puts(line);
    }
    board_puts("done\n");
    return 0;
}
