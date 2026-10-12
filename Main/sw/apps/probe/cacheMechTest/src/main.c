/******************************************************************************
*
* @file main.c: cacheMechTest
*
* @brief  缓存行为的机理实验：把 cacheCoherencyTest 依赖的几条缓存行为逐条单独测出来。
*
*         cacheCoherencyTest 回答「外部主机写入之后 CPU 读到什么」。它的判据成立
*         要靠几条关于 CPU 数据缓存本身的事实：写与读会不会装行、有没有写缓冲、
*         缓存的行号与标签取哪几位、失效指令什么时候生效、栈与 IO 访问会不会挤掉行。
*         这些事实在定判据的过程中都被猜错过，这个程序把每一条做成一组独立的判据，
*         每组自带期望值，任何一组与期望不符都计入 fail。
*
*         ## 组
*
*           T1  CPU 写后立刻读回（4096 个地址），读回值与写入值是否一致
*           T2  三种建立旧值的办法各 16 次：只写、写后读一次、别名写后读一次；
*               之后经别名改 DDR，原地址读到旧值说明这一行在缓存里
*           T3  别名写、读装行、探针写、再读，中间不做任何排空（16 次）
*           T4  装行、探针写、读 a ^ (1 << k)、读 a，k = 0..31：
*               a 被挤出说明第 k 位属于标签
*           T5  先失效、读、再让探针写、再读（8 次）：失效早于外部写入时是否有效
*           T6  读写 CSR 0x810（BSP 的写缓冲排空接口），异常号
*           T7  装行之后只做 APB 读、APB 写、探针写别处，行是否还在（行号 0 与 5）
*           T8  装行的那次读之后紧跟写栈、写别的行、读别的行、写同一行、空转后写栈
*           T9  装行与探针写之间、探针写与读之间插入寄存器空转或栈空转
*           T10 被跳过的失效指令：分支必跳，下一条是 .word(0x500F)，缓存是否被清
*           T11 失效指令的代价，计数只放寄存器，拆出「清缓存」与「重新装一行」两部分
*
*         每组开头都先失效全部缓存，组与组之间不串状态。
*
*         ## 地址
*
*         每组用不同的行号段（见各组的 ROWADDR），测试区在 0x0C81_0000 以上。
*         栈顶所在的行号在启动时打印（sp_row），它若落进某组的行号段，
*         那一组可能被栈访问挤掉，要先换地址再看结果。
*
******************************************************************************/

#include <stdint.h>
#include "bsp.h"
#include "vexriscv.h"

#define PRB        0xF8101000u
#define R_CTRL     0x00
#define R_STATUS   0x04
#define R_ADDR     0x08
#define R_LEN      0x0C
#define R_COUNT    0x10
#define R_MODE     0x14
#define R_SEED     0x18
#define R_ID       0x3C
#define ST_DONE    0x02u
#define ST_BAD     0x1Cu          /* TIMEOUT | RESP_ERR | LAST_ERR */

#define SEED       0x1357A5C0u
#define ALIAS      0x10000000u    /* 翻转 bit28：DDR 别名，同一颗粒地址、不同标签 */
#define EVICT      0x20000000u    /* 翻转 bit29：行号不变、标签不同 */
#define ROWADDR(base, row, page) ((base) + (page) * 0x1000u + (row) * 0x40u)

static int fail = 0;

static inline uint32_t rd(uint32_t off)          { return *(volatile uint32_t *)(PRB + off); }
static inline void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(PRB + off) = v; }
static inline uint32_t fresh(uint32_t a)         { return SEED + a; }

static void check(const char *tag, int ok)
{
    bsp_printf("%s %s\r\n", tag, ok ? "ok" : "FAIL");
    if (!ok)
        fail++;
}

static uint32_t probe_write(uint32_t a)
{
    wr(R_ADDR, a); wr(R_LEN, 0); wr(R_COUNT, 1); wr(R_MODE, 1); wr(R_SEED, SEED); wr(R_CTRL, 1);
    uint32_t st;
    do { st = rd(R_STATUS); } while (!(st & ST_DONE));
    return st;
}

__attribute__((noinline)) static uint32_t reg_spin(uint32_t n)
{
    uint32_t acc = 1;
    for (uint32_t j = 0; j < n; j++)
        acc = acc * 3u + j;
    return acc;
}

/* 汇编辅助：a0 = 1 时分支必跳，下一条在架构上不执行；a1 = 被观测地址 */
void skip_all_0(uint32_t take, uint32_t addr);
void skip_all_1nop(uint32_t take, uint32_t addr);
void skip_all_2nop(uint32_t take, uint32_t addr);
void skip_addr_0(uint32_t take, uint32_t addr);
void skip_nop_ctrl(uint32_t take, uint32_t addr);

/* 汇编辅助：CSR 探测。trap_rec 记下 mcause/mtval，跳过出错指令返回 */
uint32_t g_mcause = 0xFFFFFFFFu;
uint32_t g_mtval  = 0xFFFFFFFFu;
void     trap_rec(void);
uint32_t try_csrr_810(void);
void     try_csrw_810(void);
uint32_t get_mtvec(void);
void     set_mtvec(uint32_t);

__asm__(
    ".text\n.align 2\n"
    ".globl skip_all_0\nskip_all_0:\n  bnez a0, 1f\n  .word 0x500F\n1: ret\n"
    ".globl skip_all_1nop\nskip_all_1nop:\n  bnez a0, 1f\n  nop\n  .word 0x500F\n1: ret\n"
    ".globl skip_all_2nop\nskip_all_2nop:\n  bnez a0, 1f\n  nop\n  nop\n  .word 0x500F\n1: ret\n"
    ".globl skip_addr_0\nskip_addr_0:\n  bnez a0, 1f\n  .word 0x5D00F\n1: ret\n"
    ".globl skip_nop_ctrl\nskip_nop_ctrl:\n  bnez a0, 1f\n  nop\n1: ret\n"
    ".globl trap_rec\ntrap_rec:\n"
    "  csrr t0, mcause\n  la t1, g_mcause\n  sw t0, 0(t1)\n"
    "  csrr t0, mtval\n  la t1, g_mtval\n  sw t0, 0(t1)\n"
    "  csrr t0, mepc\n  addi t0, t0, 4\n  csrw mepc, t0\n  mret\n"
    ".globl try_csrr_810\ntry_csrr_810:\n  li a0, 0xDEADBEEF\n  csrr a0, 0x810\n  ret\n"
    ".globl try_csrw_810\ntry_csrw_810:\n  li t2, 1\n  csrw 0x810, t2\n  ret\n"
    ".globl get_mtvec\nget_mtvec:\n  csrr a0, mtvec\n  ret\n"
    ".globl set_mtvec\nset_mtvec:\n  csrw mtvec, a0\n  ret\n");

/* T1 */
static void t1_write_then_read(void)
{
    uint32_t bad = 0;
    data_cache_invalidate_all();
    for (uint32_t i = 0; i < 4096u; i++) {
        uint32_t a = 0x0C900000u + i * 0x1000u + ((i * 0x44u) & 0xFFCu);
        uint32_t v = 0xA5000000u ^ (i * 2654435761u);
        *(volatile uint32_t *)a = v;
        if (*(volatile uint32_t *)a != v)
            bad++;
    }
    bsp_printf("T1 write_then_read n=4096 mismatch=%d\r\n", bad);
    check("T1", bad == 0);
}

/* T2 */
static void t2_how_a_line_gets_loaded(void)
{
    uint32_t w0 = 0, w1 = 0, r1 = 0;
    data_cache_invalidate_all();
    for (uint32_t k = 0; k < 16u; k++) {
        uint32_t a = ROWADDR(0x0C810000u, 8u + k, k);
        (void)*(volatile uint32_t *)(a ^ EVICT);          /* 行先被别的标签占住 */
        *(volatile uint32_t *)a = 0x0000A000u + k;        /* 只写 */
        *(volatile uint32_t *)(a ^ ALIAS) = 0x0000B000u + k;
        if (*(volatile uint32_t *)a == 0x0000A000u + k) w0++;
    }
    for (uint32_t k = 0; k < 16u; k++) {
        uint32_t a = ROWADDR(0x0C820000u, 8u + k, k);
        (void)*(volatile uint32_t *)(a ^ EVICT);
        *(volatile uint32_t *)a = 0x0000A100u + k;        /* 写后读一次 */
        (void)*(volatile uint32_t *)a;
        *(volatile uint32_t *)(a ^ ALIAS) = 0x0000B100u + k;
        if (*(volatile uint32_t *)a == 0x0000A100u + k) w1++;
    }
    for (uint32_t k = 0; k < 16u; k++) {
        uint32_t a = ROWADDR(0x0C830000u, 8u + k, k);
        (void)*(volatile uint32_t *)(a ^ EVICT);
        *(volatile uint32_t *)(a ^ ALIAS) = 0x0000A200u + k;  /* 别名写后读一次 */
        (void)*(volatile uint32_t *)a;
        *(volatile uint32_t *)(a ^ ALIAS) = 0x0000B200u + k;
        if (*(volatile uint32_t *)a == 0x0000A200u + k) r1++;
    }
    bsp_printf("T2 stale/16 write_only=%d write_then_read=%d alias_then_read=%d\r\n", w0, w1, r1);
    check("T2", w0 == 0 && w1 == 16 && r1 == 16);
}

/* T3 */
static void t3_no_drain(void)
{
    uint32_t armed = 0, stale = 0, bad = 0;
    data_cache_invalidate_all();
    for (uint32_t k = 0; k < 16u; k++) {
        uint32_t a = ROWADDR(0x0C840000u, 48u + k, k);
        uint32_t old = 0xC0DE3000u + k;
        (void)*(volatile uint32_t *)(a ^ EVICT);
        *(volatile uint32_t *)(a ^ ALIAS) = old;
        if (*(volatile uint32_t *)a == old) armed++;
        if (probe_write(a) & ST_BAD) bad++;
        if (*(volatile uint32_t *)a == old) stale++;
    }
    bsp_printf("T3 no_drain n=16 armed=%d stale_after_probe=%d probe_bad=%d\r\n", armed, stale, bad);
    check("T3", armed == 16 && stale == 16 && bad == 0);
}

/* T4 */
static void t4_geometry(void)
{
    uint32_t evict_mask = 0, armed_mask = 0, bad = 0;
    uint32_t a = ROWADDR(0x0C850000u, 56u, 0u);
    for (uint32_t k = 0; k < 32u; k++) {
        data_cache_invalidate_all();
        uint32_t old = 0xC0DE4000u + k;
        *(volatile uint32_t *)(a ^ ALIAS) = old;
        if (*(volatile uint32_t *)a == old) armed_mask |= 1u << k;
        if (probe_write(a) & ST_BAD) bad++;
        (void)*(volatile uint8_t *)(a ^ (1u << k));
        uint32_t g = *(volatile uint32_t *)a;
        if (g == fresh(a)) evict_mask |= 1u << k;
        else if (g != old) bad++;
    }
    bsp_printf("T4 a=%x armed_mask=%x evict_mask=%x bad=%d\r\n", a, armed_mask, evict_mask, bad);
    check("T4", armed_mask == 0xFFFFFFFFu && evict_mask == 0xFFFFF000u && bad == 0);
}

/* T5 */
static void t5_invalidate_too_early(void)
{
    uint32_t stale = 0;
    for (uint32_t k = 0; k < 8u; k++) {
        uint32_t a = ROWADDR(0x0C860000u, 32u + k, k);
        uint32_t old = 0xC0DE5000u + k;
        *(volatile uint32_t *)(a ^ ALIAS) = old;
        data_cache_invalidate_all();
        (void)*(volatile uint32_t *)a;       /* 失效之后、外部写之前读了一次 */
        (void)probe_write(a);
        if (*(volatile uint32_t *)a == old) stale++;
    }
    bsp_printf("T5 invalidate_before_external_write n=8 stale=%d\r\n", stale);
    check("T5", stale == 8);
}

/* T6 */
static void t6_csr_810(void)
{
    uint32_t old_mtvec = get_mtvec();
    set_mtvec((uint32_t)trap_rec);
    g_mcause = 0xFFFFFFFFu; g_mtval = 0xFFFFFFFFu;
    uint32_t v = try_csrr_810();
    uint32_t rcause = g_mcause, rtval = g_mtval;
    g_mcause = 0xFFFFFFFFu; g_mtval = 0xFFFFFFFFu;
    try_csrw_810();
    uint32_t wcause = g_mcause, wtval = g_mtval;
    set_mtvec(old_mtvec);
    bsp_printf("T6 csrr_0x810 a0=%x mcause=%x mtval=%x\r\n", v, rcause, rtval);
    bsp_printf("T6 csrw_0x810 mcause=%x mtval=%x mtvec_restored=%x\r\n", wcause, wtval, old_mtvec);
    check("T6", rcause == 2u && wcause == 2u && v == 0xDEADBEEFu);
}

/* T7 */
static void t7_io_does_not_evict(void)
{
    uint32_t res[2][4];
    int ok = 1;
    for (uint32_t ri = 0; ri < 2u; ri++) {
        uint32_t row = ri ? 5u : 0u;
        for (uint32_t mode = 0; mode < 4u; mode++) {
            uint32_t hits = 0;
            for (uint32_t k = 0; k < 8u; k++) {
                uint32_t a = ROWADDR(0x0C8A0000u, row, (mode * 16u + k));
                uint32_t old = 0xC0DE7000u + k, nw = 0xC0DE7800u + k;
                data_cache_invalidate_all();
                *(volatile uint32_t *)(a ^ ALIAS) = old;
                if (*(volatile uint32_t *)a != old) continue;
                if (mode == 1) { for (int j = 0; j < 8; j++) (void)rd(R_STATUS); }
                if (mode == 2) { for (int j = 0; j < 8; j++) wr(R_SEED, SEED); }
                if (mode == 3) { (void)probe_write(a ^ 0x100000u); }
                *(volatile uint32_t *)(a ^ ALIAS) = nw;
                if (*(volatile uint32_t *)a == old) hits++;
            }
            res[ri][mode] = hits;
            if (hits != 8u) ok = 0;
        }
    }
    bsp_printf("T7 stale/8 [none io_rd io_wr probe_write_elsewhere] row0: %d %d %d %d  row5: %d %d %d %d\r\n",
               res[0][0], res[0][1], res[0][2], res[0][3], res[1][0], res[1][1], res[1][2], res[1][3]);
    check("T7", ok);
}

/* T8 */
static void t8_access_right_after_load(void)
{
    uint32_t hits[6] = {0};
    volatile uint32_t loc = 0;
    int ok = 1;
    for (uint32_t mode = 0; mode < 6u; mode++) {
        for (uint32_t k = 0; k < 8u; k++) {
            uint32_t a = ROWADDR(0x0C8C0000u, 36u, mode * 16u + k);
            uint32_t o = ROWADDR(0x0C8D0000u, 20u, mode * 16u + k);
            uint32_t old = 0xC0DE8000u + k, nw = 0xC0DE8800u + k;
            data_cache_invalidate_all();
            *(volatile uint32_t *)(a ^ ALIAS) = old;
            uint32_t g = *(volatile uint32_t *)a;
            if (mode == 1) loc = g;
            if (mode == 2) *(volatile uint32_t *)o = g;
            if (mode == 3) (void)*(volatile uint32_t *)o;
            if (mode == 4) *(volatile uint32_t *)(a + 8u) = g;
            if (mode == 5) { for (volatile int j = 0; j < 4; j++) ; loc = g; }
            *(volatile uint32_t *)(a ^ ALIAS) = nw;
            if (*(volatile uint32_t *)a == old) hits[mode]++;
        }
        if (hits[mode] != 8u) ok = 0;
    }
    bsp_printf("T8 stale/8 [none wr_stack wr_other_row rd_other_row wr_same_line spin_then_wr_stack] %d %d %d %d %d %d\r\n",
               hits[0], hits[1], hits[2], hits[3], hits[4], hits[5]);
    check("T8", ok);
}

/* T9 */
static void t9_delays(void)
{
    uint32_t hits[5] = {0};
    int ok = 1;
    for (uint32_t mode = 0; mode < 5u; mode++) {
        for (uint32_t k = 0; k < 8u; k++) {
            uint32_t a = ROWADDR(0x0C8E0000u, 36u, mode * 16u + k);
            uint32_t old = 0xC0DE9000u + k;
            data_cache_invalidate_all();
            *(volatile uint32_t *)(a ^ ALIAS) = old;
            (void)*(volatile uint32_t *)a;
            if (mode == 1) (void)reg_spin(100000u);
            if (mode == 2) { for (volatile uint32_t j = 0; j < 20000u; j++) ; }
            (void)probe_write(a);
            if (mode == 3) (void)reg_spin(100000u);
            if (mode == 4) { for (volatile uint32_t j = 0; j < 20000u; j++) ; }
            if (*(volatile uint32_t *)a == old) hits[mode]++;
        }
        if (hits[mode] != 8u) ok = 0;
    }
    bsp_printf("T9 stale/8 [none regspin_before stackspin_before regspin_after stackspin_after] %d %d %d %d %d\r\n",
               hits[0], hits[1], hits[2], hits[3], hits[4]);
    check("T9", ok);
}

/* T10 */
static void t10_skipped_invalidate(void)
{
    uint32_t hits[6] = {0};
    for (uint32_t mode = 0; mode < 6u; mode++) {
        for (uint32_t k = 0; k < 8u; k++) {
            uint32_t a = ROWADDR(0x0C8F0000u, 36u, mode * 16u + k);
            uint32_t old = 0xC0DEB000u + k, nw = 0xC0DEB800u + k;
            data_cache_invalidate_all();
            *(volatile uint32_t *)(a ^ ALIAS) = old;
            (void)*(volatile uint32_t *)a;
            if (mode == 1) skip_all_0(1u, a);
            if (mode == 2) skip_all_1nop(1u, a);
            if (mode == 3) skip_all_2nop(1u, a);
            if (mode == 4) skip_addr_0(1u, a);
            if (mode == 5) skip_nop_ctrl(1u, a);
            *(volatile uint32_t *)(a ^ ALIAS) = nw;
            if (*(volatile uint32_t *)a == old) hits[mode]++;
        }
    }
    bsp_printf("T10 stale/8 [none skip_all skip_all_1nop skip_all_2nop skip_addr nop_ctrl] %d %d %d %d %d %d\r\n",
               hits[0], hits[1], hits[2], hits[3], hits[4], hits[5]);
    /* 期望：跳过的失效指令紧跟分支时仍清缓存（0），隔两条 nop 不清（8）。
       隔一条 nop 的结果只记录，不作判定 */
    check("T10", hits[0] == 8u && hits[1] == 0u && hits[3] == 8u && hits[4] == 0u && hits[5] == 8u);
}

/* T11 失效代价：循环计数只放寄存器，循环体里不访问栈 */
#define TN 1000u

__attribute__((noinline)) static uint32_t loop_empty(void)
{
    uint32_t t0 = clint_getTimeLow(BSP_CLINT);
    for (uint32_t k = 0; k < TN; k++)
        __asm__("nop");
    return clint_getTimeLow(BSP_CLINT) - t0;
}

__attribute__((noinline)) static uint32_t loop_all(void)
{
    uint32_t t0 = clint_getTimeLow(BSP_CLINT);
    for (uint32_t k = 0; k < TN; k++) {
        __asm__("nop");
        data_cache_invalidate_all();
    }
    return clint_getTimeLow(BSP_CLINT) - t0;
}

__attribute__((noinline)) static uint32_t loop_addr(uint32_t a)
{
    uint32_t t0 = clint_getTimeLow(BSP_CLINT);
    for (uint32_t k = 0; k < TN; k++) {
        __asm__("nop");
        data_cache_invalidate_address(a);
    }
    return clint_getTimeLow(BSP_CLINT) - t0;
}

/* 每次循环两次未命中读：a 与 a^EVICT 同行号、不同标签，互相挤出 */
__attribute__((noinline)) static uint32_t loop_two_misses(uint32_t a)
{
    uint32_t acc = 0;
    uint32_t t0 = clint_getTimeLow(BSP_CLINT);
    for (uint32_t k = 0; k < TN; k++) {
        acc += *(volatile uint32_t *)a;
        acc += *(volatile uint32_t *)(a ^ EVICT);
    }
    return (clint_getTimeLow(BSP_CLINT) - t0) + (acc & 0u);
}

/* 每次循环两次命中读：同一地址 */
__attribute__((noinline)) static uint32_t loop_two_hits(uint32_t a)
{
    uint32_t acc = 0;
    uint32_t t0 = clint_getTimeLow(BSP_CLINT);
    for (uint32_t k = 0; k < TN; k++) {
        acc += *(volatile uint32_t *)a;
        acc += *(volatile uint32_t *)(a + 4u);
    }
    return (clint_getTimeLow(BSP_CLINT) - t0) + (acc & 0u);
}

static void t11_cost(void)
{
    uint32_t a = ROWADDR(0x0C8B0000u, 44u, 0u);
    (void)*(volatile uint32_t *)a;
    uint32_t e = loop_empty();
    uint32_t all = loop_all();
    uint32_t addr = loop_addr(a);
    uint32_t hit = loop_two_hits(a);
    uint32_t miss = loop_two_misses(a);
    bsp_printf("T11 n=%d ticks empty=%d all=%d addr=%d two_hits=%d two_misses=%d\r\n",
               TN, e, all, addr, hit, miss);
    bsp_printf("T11 per_call_x1000 all=%d addr=%d miss_minus_hit_per_read=%d\r\n",
               (all - e), (addr - e), (miss - hit) / 2u);
}

void main()
{
    volatile uint32_t stackvar = 0;
    uint32_t sp = (uint32_t)&stackvar;
    bsp_printf("cacheMechTest\r\n");
    bsp_printf("IDENT id=%x sp=%x sp_row=%d\r\n", rd(R_ID), sp, (sp >> 6) & 63u);
    if (rd(R_ID) != 0x41584950u) {
        bsp_printf("probe absent, stop\r\nfail 1\r\ndone\r\n");
        return;
    }

    t1_write_then_read();
    t2_how_a_line_gets_loaded();
    t3_no_drain();
    t4_geometry();
    t5_invalidate_too_early();
    t7_io_does_not_evict();
    t8_access_right_after_load();
    t9_delays();
    t10_skipped_invalidate();
    t11_cost();
    t6_csr_810();

    bsp_printf("fail %d\r\n", fail);
    bsp_printf("done\r\n");
}
