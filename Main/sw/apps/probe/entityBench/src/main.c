/******************************************************************************
*
* @file main.c: entityBench —— CPU 每弹幕开销的板上专项探针（前期研究版）
*
* @brief  要回答的是「每个弹幕每帧的 CPU 开销」与「布局（AoS/SoA）与工作集大小
*         对它的影响」。本程序是它的第一版可运行骨架，先立住三件事：
*
*         1. GOLD。板上的实体字段摘要与命令表摘要，与主机侧
*            `Main/tools/check/e8_entity_golden.c` 生成的 `src/golden.h` 逐项对账。
*            两边编的是**同一份 lib/core/entity.c**、同样的帧数与种子；
*            对上了才说明「板上量的确实是这个算法」。
*            这一条覆盖逻辑字段与命令记录；实体尺寸由静态断言约束。
*         2. THR。A~D 四组的每弹幕每帧周期数：A 纯更新/AoS、B 纯更新/SoA、
*            C 更新并提交/SoA、D 规模扫描（N=1000/2000/5000/10000）。
*            打点只包住更新循环，摘要与串口都在区间外。
*         3. 口径。补入次数、实际提交条数、每帧周期与 1000 帧的累计都要能对上；
*            缓存失效只在整组开始前做一次，不做逐帧失效（校验会改变缓存状态，
*            各组的预热策略必须相同）。
*
*         本版**不含**（留给正式开发）：8KB/2 路与 16KB/4 路两份比特流、
*         逐帧消费式校验（现在只在整组结束时校验一次）、
*         折帧（弹幕运动卸载已定案不做）、设计文档 1.3 节的更正。
*         同一产物的三次跨会话 2% 判据已由外部板上日志完成。
*
*         被测对象的口径见 `lib/core/entity.h` 的文件头。
*         计时口径：`board_cycles()` 是 CLINT 低 32 位、1 tick = 10ns、42.9 秒回绕。
*         本程序逐帧取一次区间（32 位相减）再累加进 64 位，不拿相距很远的读数相减。
*
******************************************************************************/

#include <stdint.h>

#include "board.h"
#include "memmap.h"

#include "core/crc32.h"
#include "core/entity.h"
#include "core/format.h"

#include "golden.h"

/* --- 内存布局（AoS、SoA、原始命令缓冲均在 DDR 共享区）------------------------
   .bss 也在 DDR；这里使用显式分区，是为了固定数组地址与相互间距，
   避免链接布局变化直接挪动被测数据。 */
#define ENT_AOS_OFF     0x00000000u     /* 共享区 +0：AoS，10000 x 20B = 200KB */
#define ENT_SOA_OFF     0x00080000u     /* 共享区 +512KB：SoA，10000 x 20B = 200KB */
#define ENT_CMD_OFF     0x00200000u     /* 共享区 +2MB：命令表，10002 条 x 32B */

#define ENT_MAX_N       10000u
#define ENT_CMD_BYTES   (10002u * 32u)

static eb_ent  *s_aos;
static uint8_t *s_soa_mem;
static uint32_t *s_cmd;

static int s_fail;

/* --- 输出 ---------------------------------------------------------------- */

static char     s_line[200];
static core_out s_out;

static void out_begin(void)
{
    core_out_init(&s_out, s_line, (uint32_t)sizeof s_line);
}

static void out_end(void)
{
    core_out_str(&s_out, "\r\n");
    board_puts(s_line);
}

static void say(const char *s)
{
    out_begin();
    core_out_str(&s_out, s);
    out_end();
}

static void f_num(uint32_t v)
{
    core_out_u32(&s_out, v);
}

static void f_hex(uint32_t v)
{
    core_out_str(&s_out, "0x");
    core_out_hex32(&s_out, v);
}

/* 周期数按千周期打印，读表时不必数零。 */
static void f_kcyc(uint64_t cycles)
{
    core_out_u32(&s_out, (uint32_t)(cycles / 1000u));
}

/* --- 一组的运行 ----------------------------------------------------------- */

typedef struct {
    uint64_t cyc;           /* 1000 帧的累计周期 */
    uint32_t cyc_min;
    uint32_t cyc_max;
    uint32_t respawn;
    uint32_t cmd_used;
    uint32_t digest_a;      /* AoS 布局的摘要（用例是 AoS 时有效） */
    uint32_t digest_s;      /* SoA 布局的摘要 */
    uint32_t digest_c;      /* 命令表摘要 */
    uint32_t bad;
} run_out;

/* 跑一组：先按种子初始化，再逐帧更新、逐帧打点。
   初始化与摘要都在计时区间之外。 */
static void run_case(const eb_golden *g, run_out *o)
{
    const int do_cmd = (g->with_cmd != 0u);
    uint32_t  rng = 0x2468ace1u;        /* 与 e8_entity_golden.c 的 RNG_SEED 同值 */
    uint32_t  f, i;
    int32_t  *x = 0, *y = 0, *vx = 0, *vy = 0;
    uint16_t *sprite = 0, *flags = 0;
    uint32_t used = 0u;

    o->cyc = 0u;
    o->cyc_min = 0xFFFFFFFFu;
    o->cyc_max = 0u;
    o->respawn = 0u;
    o->cmd_used = 0u;
    o->bad = 0u;

    /* 起点一致：整组开始前清一次数据缓存。
       不逐帧清——各组的预热与冷启动策略必须相同，逐帧清会把
       「装行」的成本灌进每一帧，量到的是另一种东西。 */
    board_dcache_invalidate_all();

    if (g->aos) {
        for (i = 0u; i < g->n; i++)
            eb_seed(&s_aos[i], &rng);
    } else {
        eb_soa_split(s_soa_mem, g->n, &x, &y, &vx, &vy, &sprite, &flags);
        for (i = 0u; i < g->n; i++)
            eb_seed_soa(i, x, y, vx, vy, sprite, flags, &rng);
    }

    for (f = 0u; f < EB_G_FRAMES; f++) {
        const uint32_t t0 = board_cycles();
        uint32_t       dt;

        if (g->aos)
            eb_frame_aos(s_aos, g->n, &rng, &o->respawn);
        else if (do_cmd)
            used = eb_frame_soa_cmd(x, y, vx, vy, sprite, flags, g->n, &rng,
                                    s_cmd, &o->respawn);
        else
            eb_frame_soa(x, y, vx, vy, sprite, flags, g->n, &rng, &o->respawn);

        dt = board_cycles() - t0;
        o->cyc += dt;
        if (dt < o->cyc_min)
            o->cyc_min = dt;
        if (dt > o->cyc_max)
            o->cyc_max = dt;
    }

    /* 摘要（计时外）：与主机侧的期望值对账。 */
    if (g->aos)
        o->digest_a = eb_digest_aos(s_aos, g->n);
    else
        o->digest_s = eb_digest_soa(x, y, vx, vy, sprite, flags, g->n);

    if (do_cmd) {
        o->cmd_used = used;
        o->digest_c = eb_digest_cmd(s_cmd, used * EB_CMD_WORDS);
    }

    /* 对账：补入次数与提交条数必须与主机侧一致（摘要另有断言，见调用处）。 */
    if (o->respawn != g->respawn)
        o->bad++;
    if (do_cmd && o->cmd_used != g->cmd_used)
        o->bad++;
}

/* 独立复核遍：每帧在计时区间外读完六个实体字段及当帧全部命令字节。
   与计时遍分开，避免 CRC 遍历改变下一帧的缓存热度与历史性能口径。 */
static void verify_frames(const eb_golden *g)
{
    uint32_t rng = 0x2468ace1u, respawn = 0u;
    uint32_t f, i, used = 0u, bad_state = 0u, bad_cmd = 0u;
    uint32_t first_bad = EB_G_FRAMES;
    int32_t *x = 0, *y = 0, *vx = 0, *vy = 0;
    uint16_t *sprite = 0, *flags = 0;

    if (g->aos) {
        for (i = 0u; i < g->n; i++)
            eb_seed(&s_aos[i], &rng);
    } else {
        eb_soa_split(s_soa_mem, g->n, &x, &y, &vx, &vy, &sprite, &flags);
        for (i = 0u; i < g->n; i++)
            eb_seed_soa(i, x, y, vx, vy, sprite, flags, &rng);
    }

    for (f = 0u; f < EB_G_FRAMES; f++) {
        uint32_t state_crc;

        if (g->aos)
            eb_frame_aos(s_aos, g->n, &rng, &respawn);
        else if (g->with_cmd)
            used = eb_frame_soa_cmd(x, y, vx, vy, sprite, flags, g->n,
                                    &rng, s_cmd, &respawn);
        else
            eb_frame_soa(x, y, vx, vy, sprite, flags, g->n, &rng, &respawn);

        state_crc = g->aos ? eb_digest_aos(s_aos, g->n)
                           : eb_digest_soa(x, y, vx, vy, sprite, flags, g->n);
        if (state_crc != g->frame_state[f]) {
            bad_state++;
            if (first_bad == EB_G_FRAMES)
                first_bad = f;
        }
        if (g->with_cmd) {
            uint32_t cmd_crc = eb_digest_cmd(s_cmd, used * EB_CMD_WORDS);
            if (used != g->n || cmd_crc != g->frame_cmd[f]) {
                bad_cmd++;
                if (first_bad == EB_G_FRAMES)
                    first_bad = f;
            }
        }
    }
    if (respawn != g->respawn) {
        bad_state++;
        if (first_bad == EB_G_FRAMES)
            first_bad = EB_G_FRAMES - 1u;
    }
    s_fail += (int)(bad_state + bad_cmd);

    out_begin();
    core_out_str(&s_out, "VERIFY-FRAMES ");
    core_out_str(&s_out, g->name);
    core_out_str(&s_out, " frames ");
    f_num(EB_G_FRAMES);
    core_out_str(&s_out, " state_bad ");
    f_num(bad_state);
    core_out_str(&s_out, " cmd_bad ");
    f_num(bad_cmd);
    if (first_bad != EB_G_FRAMES) {
        core_out_str(&s_out, " first_bad ");
        f_num(first_bad);
    }
    core_out_str(&s_out, first_bad == EB_G_FRAMES ? " ok" : " FAIL");
    out_end();
}

static void report(const eb_golden *g, const run_out *o, int check_digest)
{
    const uint32_t n  = g->n;
    const uint64_t px = (uint64_t)n * EB_G_FRAMES;
    const int      do_cmd = (g->with_cmd != 0u);
    uint64_t       per_ent_x100;

    if (check_digest) {
        if (g->aos && o->digest_a != g->aos_digest)
            s_fail++;
        if (!g->aos && o->digest_s != g->soa_digest)
            s_fail++;
        if (do_cmd && o->digest_c != g->cmd_digest)
            s_fail++;
    }
    if (o->bad != 0u)
        s_fail += o->bad;

    per_ent_x100 = (px != 0u) ? (o->cyc * 100u) / px : 0u;

    out_begin();
    core_out_str(&s_out, g->name);
    core_out_str(&s_out, " n ");
    f_num(n);
    core_out_str(&s_out, " cyc_k ");
    f_kcyc(o->cyc);
    core_out_str(&s_out, " us/frame ");
    f_num((uint32_t)(o->cyc / EB_G_FRAMES / 100u));
    core_out_str(&s_out, " c/ent/frame ");
    f_num((uint32_t)per_ent_x100 / 100u);
    core_out_ch(&s_out, '.');
    if ((uint32_t)per_ent_x100 % 100u < 10u)
        core_out_ch(&s_out, '0');
    f_num((uint32_t)per_ent_x100 % 100u);
    core_out_str(&s_out, " fmin ");
    f_num(o->cyc_min);
    core_out_str(&s_out, " fmax ");
    f_num(o->cyc_max);
    core_out_str(&s_out, " resp ");
    f_num(o->respawn);
    if (do_cmd) {
        core_out_str(&s_out, " cmd ");
        f_num(o->cmd_used);
    }
    if (check_digest) {
        core_out_str(&s_out, " crc ");
        f_hex(g->aos ? o->digest_a : o->digest_s);
        if (do_cmd) {
            core_out_ch(&s_out, '/');
            f_hex(o->digest_c);
        }
    }
    if (o->bad != 0u)
        core_out_str(&s_out, " FAIL");
    out_end();
}

/* --- 标定：把「打点自身的成本」与「纯循环成本」从每弹幕数字里摘出来 --------
   E8 报的是每弹幕每帧周期数，而每帧一次 `board_cycles()` 差是打点动作本身；
   不标定它就会把「每帧的固定开销 / N」算进每弹幕成本，N 越小偏得越多。
   标定只在启动时做一次，不进测量区。 */
#define CAL_ITER    10000u

static uint32_t s_cal_cyc_call;
static uint32_t s_cal_cyc_loop;
static uint32_t s_cal_cyc_read;

static void calibrate(void)
{
    uint32_t t0, i, acc = 0u;

    /* 1) 连着两次读数：一帧一次打点的成本就是这个差。 */
    t0 = board_cycles();
    s_cal_cyc_call = board_cycles() - t0;

    /* 2) 空循环：只有循环变量与比较。 */
    t0 = board_cycles();
    for (i = 0u; i < CAL_ITER; i++)
        acc += i;
    s_cal_cyc_loop = board_cycles() - t0;

    /* 3) 带取模寻址与累加的诊断循环；不能据此单独推断访存延迟。 */
    t0 = board_cycles();
    for (i = 0u; i < CAL_ITER; i++)
        acc += (uint32_t)s_aos[i % 1000u].x;
    s_cal_cyc_read = board_cycles() - t0;

    if (acc == 0xDEADBEEFu)         /* 防优化：这一支永不为真 */
        say("cal sink");
}

/* --- main ---------------------------------------------------------------- */

int main(void)
{
    uint32_t i;

    board_init();
    crc32_init();

    say("");
    say("entityBench E8 前期研究版");
    out_begin();
    core_out_str(&s_out, "BOARD_HZ ");
    f_num(BOARD_HZ);
    core_out_str(&s_out, " frames ");
    f_num(EB_G_FRAMES);
    core_out_str(&s_out, " cases ");
    f_num(EB_G_N);
    out_end();

    /* 实体池与命令表：指针直接取分区偏移（板上 CPU 地址 == 控制器地址，恒等关系已实测）。 */
    s_aos     = (eb_ent *)(uintptr_t)(g_mem_regions[REGION_SHARED].offset + ENT_AOS_OFF);
    s_soa_mem = (uint8_t *)(uintptr_t)(g_mem_regions[REGION_SHARED].offset + ENT_SOA_OFF);
    s_cmd     = (uint32_t *)(uintptr_t)(g_mem_regions[REGION_SHARED].offset + ENT_CMD_OFF);

    if (ENT_CMD_OFF + ENT_CMD_BYTES
            > g_mem_regions[REGION_SHARED].size) {
        say("win err 命令表越过共享区");
        say("fail 1");
        say("done");
        return 0;
    }

    /* 扫描输出保持开启：每弹幕开销要与像素吞吐基线在同一平台上量，
       扫描的访存不能停掉（否则量出来的每弹幕开销偏乐观）。 */
    board_present(0);

    calibrate();
    out_begin();
    core_out_str(&s_out, "CAL cyc/pair-call ");
    f_num(s_cal_cyc_call);
    core_out_str(&s_out, " cyc/empty-loop-x100 ");
    f_num((s_cal_cyc_loop * 100u) / CAL_ITER);
    core_out_str(&s_out, " cyc/x-modsum-x100 ");
    f_num((s_cal_cyc_read * 100u) / CAL_ITER);
    out_end();

    for (i = 0u; i < EB_G_N; i++) {
        const eb_golden *g = &g_golden[i];
        run_out          o;

        if (g->n > ENT_MAX_N) {
            say("case err n 超出实体池");
            s_fail++;
            continue;
        }

        run_case(g, &o);
        report(g, &o, 1 /* 对摘要 */);
        verify_frames(g);

        /* 摘要不一致时把两侧的值都打出来，便于定位是哪个字段漂了。 */
        if (g->aos && o.digest_a != g->aos_digest) {
            out_begin();
            core_out_str(&s_out, "  DIGEST-A 板 ");
            f_hex(o.digest_a);
            core_out_str(&s_out, " 期望 ");
            f_hex(g->aos_digest);
            out_end();
        }
        if (!g->aos && o.digest_s != g->soa_digest) {
            out_begin();
            core_out_str(&s_out, "  DIGEST-S 板 ");
            f_hex(o.digest_s);
            core_out_str(&s_out, " 期望 ");
            f_hex(g->soa_digest);
            out_end();
        }
        if (g->with_cmd != 0u && o.digest_c != g->cmd_digest) {
            out_begin();
            core_out_str(&s_out, "  DIGEST-C 板 ");
            f_hex(o.digest_c);
            core_out_str(&s_out, " 期望 ");
            f_hex(g->cmd_digest);
            out_end();
        }
    }

    out_begin();
    core_out_str(&s_out, "fail ");
    f_num((uint32_t)s_fail);
    out_end();
    say("done");
    return 0;
}
