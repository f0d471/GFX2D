/******************************************************************************
*
* @file main.c: displayFifoProbe
*
* @brief  实验 E5 的 2.4 节：真实扫描 FIFO 的压力矩阵与深度定值。
*
*         三个场景配置相同，只差 S1 上的探针主机是否启动：
*             idle   探针不启动，给同一比特流、同一程序下的无 S1 压力基线
*             read   MODE=0，连续 256 拍读
*             write  MODE_WRITE，连续 256 拍写
*         每个场景先停稳两侧、在事务边界 CLEAR、无压力预装 2 场，再启动压力源；
*         随后丢 2 个压力预热快照，收 120 个连续、互不重复的已结束场快照；
*         报最低水位的最小值、最大值、首次最小值的帧号与两个中位数，
*         并检查帧完整性、两个累计欠载计数与响应错误。
*         判据归档在仓库根目录 `Engineering/TechReports/platform/08-扫描FIFO的压力矩阵与深度定值.md`
*         的 3.6 节，权威寄存器表在下面三个 RTL 文件头。
*
*         **只在 Ti60 上运行**，不提供 host 构建：它要读 display / guard / axiprobe
*         三个 APB 窗口的量测寄存器，这些不是跨平台的游戏平台能力。所以寄存器偏移
*         按三个 RTL 文件头复制最小集到本文件，不改 `board.h`、`board.c` 或 host BSP
*         （理由见上述技术报告 3.2 节）；运行时先用三组 ID/VERSION 拒绝错误比特流。
*
*           窗口              模块                        表在
*           0xF810_E000       display（扫描输出）          rtl/display/display_fetch.v
*           0xF810_F000       axi_mux_guard                rtl/bus/axi_mux_guard.v
*           0xF810_1000       axi_probe_master（S1 探针）  rtl/bus/axi_probe_master.v
*
*         三条硬约束（E5 2.4.4 节）：
*           1. 所有等待都有界，常数在下面定死；周期差用无符号减法，允许 32 位回绕。
*           2. 采样期不打印串口、不做 cpu_copy()，结果先落内存，三组跑完统一打印。
*           3. 失败先留现场；健康探针先在原预算内自然收尾，只有错误/超时才 ABORT。
*
*         行格式与期望输出见 `Main/sw/README.md`「displayFifoProbe 的期望值」。
*
******************************************************************************/

#include <stdint.h>

#include "board.h"
#include "memmap.h"
#include "core/format.h"

/* --------------------------------------------------------------------------
 * 寄存器表（窗口内偏移）
 * -------------------------------------------------------------------------- */

/* display：rtl/display/display_fetch.v 文件头 */
#define DISP_BASE       0xF810E000u
#define D_ID            0x000u
#define D_VERSION       0x004u
#define D_CTRL          0x008u
#define D_FRONT         0x00Cu
#define D_MAX_FIRST     0x018u
#define D_MAX_FULL      0x01Cu
#define D_MAX_GAP       0x020u
#define D_TXN           0x024u
#define D_BEAT          0x02Cu
#define D_SKIP          0x030u
#define D_RESPERR       0x034u
#define D_STATUS        0x038u
#define D_SNAP_FRAME    0x0E0u
#define D_SNAP_MIN      0x0E4u
#define D_ZERO          0x0E8u
#define D_UNDER         0x0ECu
#define D_FRAME_BEATS   0x0F0u
#define D_END_LEVEL     0x0F4u

#define DISP_ID_VAL     0x44495350u     /* "DISP" */
#define DISP_VER_VAL    1u
#define D_CTRL_EN       (1u << 0)
#define D_CTRL_SRC      (1u << 1)
#define D_CTRL_CLEAR    (1u << 2)
#define D_ST_IN_FLIGHT  (1u << 0)
#define D_ST_AR_VALID   (1u << 1)
#define D_ST_STARTED    (1u << 2)
#define D_ST_ACTIVE     (D_ST_IN_FLIGHT | D_ST_AR_VALID | D_ST_STARTED)

/* axi_mux_guard：rtl/bus/axi_mux_guard.v 文件头 */
#define GUARD_BASE      0xF810F000u
#define G_ID            0x000u
#define G_VERSION       0x004u
#define G_CTRL          0x008u
#define G_MASK          0x00Cu
#define G_AGE           0x010u
#define G_STATUS        0x014u
#define G_HITS          0x018u
#define G_MAX_AR        0x01Cu
#define G_MAX_AW        0x020u

#define GUARD_ID_VAL    0x4D555847u     /* "MUXG" */
#define GUARD_VER_VAL   2u
#define GUARD_CTRL_VAL  3u              /* bit0 ARB_TYPE=1 优先级，bit1 GUARD_EN=1 */
#define GUARD_MASK_VAL  7u              /* S0/S1/S2 全开 */
#define GUARD_AGE_VAL   64u

/* axi_probe_master：rtl/bus/axi_probe_master.v 文件头 */
#define PRB_BASE        0xF8101000u
#define P_CTRL          0x000u
#define P_STATUS        0x004u
#define P_ADDR          0x008u
#define P_LEN           0x00Cu
#define P_COUNT         0x010u
#define P_MODE          0x014u
#define P_SEED          0x018u
#define P_TXDONE        0x020u
#define P_CYCLES        0x024u
#define P_ERRS          0x02Cu
#define P_ID            0x03Cu

#define PROBE_ID_VAL    0x41584950u     /* "AXIP" */
#define P_CTRL_START    (1u << 0)
#define P_CTRL_ABORT    (1u << 1)
#define P_ST_BUSY       (1u << 0)
#define P_ST_DONE       (1u << 1)
#define P_ST_TIMEOUT    (1u << 2)
#define P_ST_RESP_ERR   (1u << 3)
#define P_ST_LAST_ERR   (1u << 4)
/* 只要出现这三位就立刻判该场景失败，不等 DONE */
#define P_ST_FATAL      (P_ST_TIMEOUT | P_ST_RESP_ERR | P_ST_LAST_ERR)
#define P_MODE_WRITE    1u              /* bit0；bit1 INC 不置（地址固定） */

/* --------------------------------------------------------------------------
 * 定值（E5 2.4.4：常数在开工前定死）
 * -------------------------------------------------------------------------- */

#define SAMPLES             120u                    /* 每场景接受的样本数 */
#define WARMUP_FIELDS       2u                      /* 丢弃的预热快照数 */
#define BEATS_PER_FIELD     (480u * 160u)           /* 76800，一场弹出的拍数 */
#define MIN_LEVEL_FLOOR     160u                    /* 最低水位不得低于一行 */
#define FRAME_WAIT_CYCLES   (BOARD_HZ / 10u)        /* 等一帧/一份快照的上限：100 ms */
#define PROBE_RUN_CYCLES    (BOARD_HZ * 5u)         /* START 到 DONE 的总上限：5 s */
#define ABORT_WAIT_CYCLES   (BOARD_HZ / 100u)       /* ABORT 后等 BUSY 清除：10 ms */

#define PROBE_ADDR          0x0D000000u             /* 避开程序、命令表、帧缓冲与共享区 */
#define PROBE_LEN           255u                    /* 256 拍 */
#define PROBE_SEED          0x2468ACE0u
#define PROBE_COUNT_BASE    0x00100000u             /* 预期约 3.35 s */
#define PROBE_COUNT_RETRY   0x00200000u             /* 只允许翻倍重跑一次 */

/* --------------------------------------------------------------------------
 * 失败原因
 * -------------------------------------------------------------------------- */

enum {
    R_OK = 0,
    R_EARLY_DONE,       /* 采样窗内探针已 DONE：该轮作废，COUNT 翻倍重跑一次 */
    R_JUMP,             /* 快照帧号跳号：场景作废重跑 */
    R_WARMUP_UNDER,     /* 清零确认或第二个预热快照的累计欠载不为 0：直接失败 */
    R_NO_FRAME,         /* 等不到新的快照（100 ms 越界） */
    R_SAMPLE_BAD,       /* 样本本身违反帧完整性或最低水位下限 */
    R_PROBE_ERR,        /* TIMEOUT / RESP_ERR / LAST_ERR，或 ERRS 非 0 */
    R_PROBE_LATE,       /* 5 s 内没有 DONE */
    R_TX_MISMATCH,      /* TX_DONE 与 COUNT 不符 */
    R_SCAN_STOP         /* 写 EN=0 后 100 ms 内扫描仍未真正停下 */
};

static const char *why_str(int r)
{
    switch (r) {
    case R_OK:           return "ok";
    case R_EARLY_DONE:   return "early_done";
    case R_JUMP:         return "frame_jump";
    case R_WARMUP_UNDER: return "warmup_underflow";
    case R_NO_FRAME:     return "no_frame";
    case R_SAMPLE_BAD:   return "sample_bad";
    case R_PROBE_ERR:    return "probe_err";
    case R_PROBE_LATE:   return "probe_late";
    case R_TX_MISMATCH:  return "tx_mismatch";
    case R_SCAN_STOP:    return "scan_stop";
    default:             return "unknown";
    }
}

/* 只有规格明确判为“本轮无效”的两种情况可以重跑。
   欠载、坏样本、等不到帧和停不下扫描都是真失败，不能用第二轮把第一次抹掉。 */
static int retryable(int r)
{
    return (r == R_EARLY_DONE) || (r == R_JUMP);
}

/* --------------------------------------------------------------------------
 * 场景结果
 * -------------------------------------------------------------------------- */

typedef struct {
    /* 配置与结论 */
    uint32_t    count;          /* 本轮 COUNT（0 表示探针没启动） */
    uint32_t    samples;
    uint32_t    ok;
    const char *reason;
    /* 采样统计 */
    uint32_t    min, max, first_frame, med_lo, med_hi;
    uint32_t    beats_bad, end_bad, low_bad;
    /* 场景结束时的一次性读取（停扫描之后，同一刻） */
    uint32_t    zero, under, resperr, skip;
    uint32_t    d_txn, d_beat, d_max_first, d_max_full, d_max_gap, d_status;
    uint32_t    p_status, p_txdone, p_errs, p_cycles;
    uint32_t    g_status, g_hits, g_max_ar, g_max_aw;
    /* 重跑记录 */
    uint32_t    retried;
    const char *first_fail;
    /* 失败现场（ABORT 之前保存） */
    uint32_t    aborted, p_status_after;
    uint32_t    f_frame, f_min, f_zero, f_under, f_beats, f_end;
} scene;

static const scene scene_zero;      /* 全 0 模板，避免逐字段清 */

/* 采样结果（定长，只在采样结束后用，不占运行期的访存带宽） */
static uint32_t s_min[SAMPLES];
static uint16_t s_frame[SAMPLES];
static uint32_t s_sorted[SAMPLES];

static int fails;

/* --------------------------------------------------------------------------
 * APB 读写
 * -------------------------------------------------------------------------- */

static inline uint32_t rd32(uint32_t base, uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}

static inline void wr32(uint32_t base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)(base + off) = v;
}

#define drd(o)          rd32(DISP_BASE, (o))
#define dwr(o, v)       wr32(DISP_BASE, (o), (v))
#define grd(o)          rd32(GUARD_BASE, (o))
#define prd(o)          rd32(PRB_BASE, (o))
#define pwr(o, v)       wr32(PRB_BASE, (o), (v))

/* --------------------------------------------------------------------------
 * 输出（一行一行拼再一次发出去；采样期一次都不调用）
 * -------------------------------------------------------------------------- */

static char     s_line[200];
static core_out s_out;

static void lbegin(const char *s)
{
    core_out_init(&s_out, s_line, (uint32_t)sizeof s_line);
    core_out_str(&s_out, s);
}

static void ldec(const char *k, uint32_t v)
{
    core_out_str(&s_out, k);
    core_out_u32(&s_out, v);
}

static void lhex(const char *k, uint32_t v)
{
    core_out_str(&s_out, k);
    core_out_hex32(&s_out, v);
}

static void lend(void)
{
    core_out_str(&s_out, "\r\n");
    board_puts(s_line);
}

/* --------------------------------------------------------------------------
 * 开局自检：三组 ID/VERSION，加 guard 的上电配置
 * -------------------------------------------------------------------------- */

static int ident_check(void)
{
    int fail = 0;
    int ok;

    lbegin("IDENT disp id=");
    core_out_hex32(&s_out, drd(D_ID));
    lhex(" ver=", drd(D_VERSION));
    ok = (drd(D_ID) == DISP_ID_VAL) && (drd(D_VERSION) == DISP_VER_VAL);
    core_out_str(&s_out, ok ? " ok" : " FAIL");
    lend();
    fail += !ok;

    /* guard 必须是上电默认：优先级、三通道全开、保护开启、AGE_LIMIT=64。
       每次上板都从完整烧写比特流开始，所以这三项不等就是比特流不对或流程不对；
       不在这里偷偷替它写回去，那会把「配置归属」这条判据掩盖掉。 */
    lbegin("IDENT guard id=");
    core_out_hex32(&s_out, grd(G_ID));
    lhex(" ver=", grd(G_VERSION));
    lhex(" ctrl=", grd(G_CTRL));
    lhex(" mask=", grd(G_MASK));
    lhex(" age=", grd(G_AGE));
    ok = (grd(G_ID) == GUARD_ID_VAL) && (grd(G_VERSION) == GUARD_VER_VAL) &&
         (grd(G_CTRL) == GUARD_CTRL_VAL) && (grd(G_MASK) == GUARD_MASK_VAL) &&
         (grd(G_AGE) == GUARD_AGE_VAL);
    core_out_str(&s_out, ok ? " ok" : " FAIL");
    lend();
    fail += !ok;

    lbegin("IDENT probe id=");
    core_out_hex32(&s_out, prd(P_ID));
    ok = (prd(P_ID) == PROBE_ID_VAL);
    core_out_str(&s_out, ok ? " ok" : " FAIL");
    lend();
    fail += !ok;

    return fail;
}

/* 前缓冲放一幅稳定图案：八条横向色带。内容不参与判据，稳定可复现即可。 */
static void fill_pattern(void)
{
    static const uint32_t band[8] = {
        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0x00FFFFFFu,
        0x00FFFF00u, 0x0000FFFFu, 0x00FF00FFu, 0x00000000u
    };
    uint32_t *fb = board_fb(0);
    uint32_t  y, x;

    for (y = 0; y < FB_HEIGHT; y++) {
        uint32_t *row = fb + y * FB_ROW_PIXELS;
        uint32_t  c = band[y / 60u];
        for (x = 0; x < FB_WIDTH; x++)
            row[x] = c;
    }
}

/* --------------------------------------------------------------------------
 * 快照与探针
 * -------------------------------------------------------------------------- */

/* 读一组场快照；返回 1 表示读取期间帧号没变（这一组是同一场的） */
static int snap_read(uint32_t *f, uint32_t *m, uint32_t *z,
                     uint32_t *u, uint32_t *b, uint32_t *e)
{
    uint32_t f0 = drd(D_SNAP_FRAME);

    *m = drd(D_SNAP_MIN);
    *z = drd(D_ZERO);
    *u = drd(D_UNDER);
    *b = drd(D_FRAME_BEATS);
    *e = drd(D_END_LEVEL);
    *f = f0;
    return (f0 == drd(D_SNAP_FRAME));
}

/* 等 SNAP_FRAME 变化；成功返回 1，100 ms 越界返回 0 */
static int wait_snap(uint32_t seen, uint32_t *f)
{
    uint32_t t0 = board_cycles();

    for (;;) {
        uint32_t now = drd(D_SNAP_FRAME);
        if (now != seen) {
            *f = now;
            return 1;
        }
        if ((uint32_t)(board_cycles() - t0) > FRAME_WAIT_CYCLES)
            return 0;
    }
}

static void probe_start(uint32_t count, uint32_t mode)
{
    pwr(P_ADDR, PROBE_ADDR);
    pwr(P_LEN, PROBE_LEN);
    pwr(P_COUNT, count);
    pwr(P_MODE, mode);
    pwr(P_SEED, PROBE_SEED);
    pwr(P_CTRL, P_CTRL_START);
}

/* 停扫描，让 txn/beat/MAX 真正冻住。
   STATUS bit0 只表示已有 R 事务在途；只等它清零会在两笔之间过早返回，旧日志因此读到
   status=0x4（started 仍为 1）。必须等 started/ar_valid/in_flight 三位全清。 */
static int scan_off(void)
{
    uint32_t t0;

    dwr(D_CTRL, 0);
    t0 = board_cycles();
    while ((drd(D_STATUS) & D_ST_ACTIVE) != 0u) {
        if ((uint32_t)(board_cycles() - t0) > FRAME_WAIT_CYCLES)
            return 0;
    }
    return 1;
}

/* 失败现场：先把能读的都读走；健康探针自然收尾，错误/超时才 ABORT。
   prime_display 可能在探针启动前失败，probe_started 用来避免为空闲探针空等 5 秒并误报 ABORT。 */
static void scene_abort(scene *sc, int mode, uint32_t t_start, int probe_started)
{
    uint32_t t0, st;

    sc->d_status = drd(D_STATUS);
    sc->d_txn    = drd(D_TXN);
    sc->d_beat   = drd(D_BEAT);
    sc->d_max_first = drd(D_MAX_FIRST);
    sc->d_max_full  = drd(D_MAX_FULL);
    sc->d_max_gap   = drd(D_MAX_GAP);
    sc->resperr  = drd(D_RESPERR);
    sc->g_status = grd(G_STATUS);
    sc->g_hits   = grd(G_HITS);
    sc->f_frame  = drd(D_SNAP_FRAME);
    sc->f_min    = drd(D_SNAP_MIN);
    sc->f_zero   = drd(D_ZERO);
    sc->f_under  = drd(D_UNDER);
    sc->f_beats  = drd(D_FRAME_BEATS);
    sc->f_end    = drd(D_END_LEVEL);

    if ((mode >= 0) && probe_started) {
        sc->p_status = prd(P_STATUS);
        sc->p_txdone = prd(P_TXDONE);
        sc->p_errs   = prd(P_ERRS);
        sc->p_cycles = prd(P_CYCLES);
        st = sc->p_status;
        if ((st & (P_ST_DONE | P_ST_FATAL)) == 0u) {
            /* 普通判据失败不等于探针故障。先让仍在正常运行的事务于原 5 秒预算内
               自然收尾；写事务中途 ABORT 会撤销 valid，只能作为真正故障的最后救场。 */
            while ((st & (P_ST_DONE | P_ST_FATAL)) == 0u &&
                   (uint32_t)(board_cycles() - t_start) <= PROBE_RUN_CYCLES)
                st = prd(P_STATUS);
            sc->p_status = st;
            sc->p_txdone = prd(P_TXDONE);
            sc->p_errs   = prd(P_ERRS);
            sc->p_cycles = prd(P_CYCLES);
        }
        if ((st & P_ST_DONE) == 0u) {
            /* 探针自身错误或总上限越界：这时正常收尾已经不可得，才撤销 valid 救场。 */
            pwr(P_CTRL, P_CTRL_ABORT);
            sc->aborted = 1u;
            t0 = board_cycles();
            while ((prd(P_STATUS) & P_ST_BUSY) &&
                   ((uint32_t)(board_cycles() - t0) <= ABORT_WAIT_CYCLES))
                ;
            sc->p_status_after = prd(P_STATUS);
        }
    }
    (void)scan_off();
}

/* 每个场景都从同一个“统计在事务边界清零、display 已预装”的状态开始。
   1) 先真正停机，保证 CLEAR 不会切在一笔 160 拍事务中间；否则 TXN 与 BEAT 会差掉
      CLEAR 前已经收到的拍数（复现时恰好差 13 拍）。
   2) 停机状态写 CLEAR，再等一份快照确认像素侧累计值也清零。
   3) 无 S1 压力启用 display 并预装两场；若冷启动就叠加满写压力，第一场会欠载。
   4) 返回后才启动探针；正式的 2 个压力预热快照与 120 个样本口径不变。 */
static int prime_display(void)
{
    uint32_t seen = drd(D_SNAP_FRAME);
    uint32_t i, f, m, z, u, b, e;

    if (!scan_off())
        return R_SCAN_STOP;

    /* core 的 started 清零后，像素侧 src_eff 还要经过 CDC 并在场起始锁存。
       先等两场让它也确实变成 0；这段可能产生的旧欠载随后由 CLEAR 清掉。 */
    seen = drd(D_SNAP_FRAME);
    for (i = 0; i < 2u; i++) {
        if (!wait_snap(seen, &f))
            return R_NO_FRAME;
        seen = f;
    }

    dwr(D_CTRL, D_CTRL_CLEAR);          /* EN=0、SRC=0：无在途事务时清统计 */
    if (!wait_snap(seen, &seen))
        return R_NO_FRAME;
    if (!snap_read(&f, &m, &z, &u, &b, &e))
        return R_JUMP;
    if ((z != 0u) || (u != 0u))
        return R_WARMUP_UNDER;

    dwr(D_CTRL, D_CTRL_EN | D_CTRL_SRC);
    for (i = 0; i < 2u; i++) {
        if (!wait_snap(seen, &f))
            return R_NO_FRAME;
        seen = f;
    }
    if (!snap_read(&f, &m, &z, &u, &b, &e))
        return R_JUMP;
    if ((z != 0u) || (u != 0u))
        return R_WARMUP_UNDER;
    return R_OK;
}

/* 等探针正常 DONE，并核对完成笔数与错误计数 */
static int wait_probe_done(uint32_t count, uint32_t t_start, scene *sc)
{
    for (;;) {
        uint32_t st = prd(P_STATUS);
        if (st & P_ST_FATAL)
            return R_PROBE_ERR;
        if (st & P_ST_DONE)
            break;
        if ((uint32_t)(board_cycles() - t_start) > PROBE_RUN_CYCLES)
            return R_PROBE_LATE;
    }
    sc->p_status = prd(P_STATUS);
    sc->p_txdone = prd(P_TXDONE);
    sc->p_errs   = prd(P_ERRS);
    sc->p_cycles = prd(P_CYCLES);
    if (sc->p_status != P_ST_DONE)      /* DONE 之外还挂着 TIMEOUT/RESP_ERR/LAST_ERR */
        return R_PROBE_ERR;
    if (sc->p_txdone != count)
        return R_TX_MISMATCH;
    if (sc->p_errs != 0u)
        return R_PROBE_ERR;
    return R_OK;
}

/* 预热 + 收 120 个样本 */
static int sample_window(int mode, uint32_t t_start, scene *sc)
{
    uint32_t seen, f, m, z, u, b, e;
    uint32_t warm = 0, n = 0, last = 0, zero = 0, under = 0;
    int      have_last = 0;

    /* 丢 2 个完整快照：让 SRC 的场起始锁存、FIFO 预装与跨域 CLEAR 都稳定下来。
       第 2 个预热快照必须读到两个累计欠载计数为 0，否则这一组不作数。 */
    seen = drd(D_SNAP_FRAME);
    while (warm < WARMUP_FIELDS) {
        if (!wait_snap(seen, &seen))
            return R_NO_FRAME;
        if (!snap_read(&f, &m, &z, &u, &b, &e))
            continue;                   /* 读到一半跨了快照，等下一场 */
        seen = f;
        warm++;
        zero = z;
        under = u;
    }
    if ((zero != 0u) || (under != 0u))
        return R_WARMUP_UNDER;
    last = seen;
    have_last = 1;

    while (n < SAMPLES) {
        if (mode >= 0) {
            uint32_t st = prd(P_STATUS);
            if (st & P_ST_FATAL)
                return R_PROBE_ERR;
            if (!(st & P_ST_BUSY))
                return R_EARLY_DONE;    /* 采样窗内就 DONE 了：这一轮无效 */
            if ((uint32_t)(board_cycles() - t_start) > PROBE_RUN_CYCLES)
                return R_PROBE_LATE;
        }
        if (!wait_snap(seen, &seen))
            return R_NO_FRAME;
        if (!snap_read(&f, &m, &z, &u, &b, &e))
            continue;
        seen = f;

        /* 顶部那次检查只能证明等待本场之前还在 BUSY；第 120 个快照尤其要在读取后
           再检查一次，防止探针恰好在这一场内提前 DONE 却被当作覆盖了完整采样窗。 */
        if (mode >= 0) {
            uint32_t st = prd(P_STATUS);
            if (st & P_ST_FATAL)
                return R_PROBE_ERR;
            if (!(st & P_ST_BUSY))
                return R_EARLY_DONE;
            if ((uint32_t)(board_cycles() - t_start) > PROBE_RUN_CYCLES)
                return R_PROBE_LATE;
        }

        /* 相邻样本的帧号必须按 16 位模加法恰好 +1；跳号说明软件漏采 */
        if (have_last && (((uint32_t)(uint16_t)(f - last)) != 1u))
            return R_JUMP;

        if (b != BEATS_PER_FIELD)
            sc->beats_bad++;
        if (e != 0u)
            sc->end_bad++;
        if (m < MIN_LEVEL_FLOOR)
            sc->low_bad++;

        s_min[n]   = m;
        s_frame[n] = (uint16_t)f;
        n++;
        last = f;
        have_last = 1;
    }

    sc->samples = n;
    if ((sc->beats_bad != 0u) || (sc->end_bad != 0u) || (sc->low_bad != 0u))
        return R_SAMPLE_BAD;
    return R_OK;
}

/* 场末累计值：读取探针 DONE 时最新的一份完整快照。
   不再强等“下一场”：它已经落在压力结束之后，不增加覆盖，反而把收尾拖进无压力区。
   若恰好跨快照就重读，整个原子读取仍受 100 ms 上限约束。 */
static int final_snapshot(scene *sc)
{
    uint32_t f, m, z, u, b, e;
    uint32_t t0 = board_cycles();

    for (;;) {
        if (snap_read(&f, &m, &z, &u, &b, &e)) {
            sc->zero  = z;
            sc->under = u;
            return 0;
        }
        if ((uint32_t)(board_cycles() - t0) > FRAME_WAIT_CYCLES)
            return -1;
    }
}

/* 场景结束时的一次性读取（已停扫描，数字不再变） */
static void read_diag(scene *sc)
{
    sc->d_txn    = drd(D_TXN);
    sc->d_beat   = drd(D_BEAT);
    sc->d_max_first = drd(D_MAX_FIRST);
    sc->d_max_full  = drd(D_MAX_FULL);
    sc->d_max_gap   = drd(D_MAX_GAP);
    sc->skip     = drd(D_SKIP);
    sc->resperr  = drd(D_RESPERR);
    sc->d_status = drd(D_STATUS);
    sc->p_status = prd(P_STATUS);
    sc->p_txdone = prd(P_TXDONE);
    sc->p_errs   = prd(P_ERRS);
    sc->p_cycles = prd(P_CYCLES);
    sc->g_status = grd(G_STATUS);
    sc->g_hits   = grd(G_HITS);
    sc->g_max_ar = grd(G_MAX_AR);
    sc->g_max_aw = grd(G_MAX_AW);
}

/* 120 个样本：最小值、最大值、首次最小值的帧号、两个中位数 */
static void sort_stats(scene *sc)
{
    uint32_t i, j, key, n = sc->samples;

    for (i = 0; i < n; i++)
        s_sorted[i] = s_min[i];
    for (i = 1; i < n; i++) {
        key = s_sorted[i];
        j = i;
        while ((j > 0u) && (s_sorted[j - 1u] > key)) {
            s_sorted[j] = s_sorted[j - 1u];
            j--;
        }
        s_sorted[j] = key;
    }
    sc->min = s_sorted[0];
    sc->max = s_sorted[n - 1u];
    /* 120 是偶数，两个中间样本都报出来，不做整数除法丢掉 0.5 拍 */
    sc->med_lo = s_sorted[n / 2u - 1u];
    sc->med_hi = s_sorted[n / 2u];
    sc->first_frame = 0;
    for (i = 0; i < n; i++) {
        if (s_min[i] == sc->min) {
            sc->first_frame = s_frame[i];
            break;
        }
    }
}

/* 一个场景的一次完整尝试 */
static int one_attempt(int mode, uint32_t count, scene *sc)
{
    uint32_t t_start;
    int      r;

    t_start = board_cycles();
    r = prime_display();
    if (r != R_OK) {
        scene_abort(sc, mode, t_start, 0);
        return r;
    }

    /* 正式场景起点：display 已预装且统计已在事务边界清零；read/write 现在启动探针，
       让负载覆盖正式的 2 个预热快照与全部 120 个样本。 */
    t_start = board_cycles();
    if (mode >= 0)
        probe_start(count, (uint32_t)mode);

    r = sample_window(mode, t_start, sc);
    if ((r == R_OK) && (mode >= 0))
        r = wait_probe_done(count, t_start, sc);
    if (r != R_OK) {
        scene_abort(sc, mode, t_start, mode >= 0);
        return r;
    }
    if (final_snapshot(sc) != 0) {
        scene_abort(sc, mode, t_start, mode >= 0);
        return R_NO_FRAME;
    }

    if (!scan_off()) {
        scene_abort(sc, mode, t_start, mode >= 0);
        return R_SCAN_STOP;
    }
    read_diag(sc);
    sort_stats(sc);
    return R_OK;
}

/*
 * 跑一个场景。允许作废重跑一次（E5 2.4.2、2.4.3）：
 * 采样窗内提前 DONE 时第二轮把 COUNT 翻倍，帧号跳变用同一 COUNT 重跑；
 * 第二轮再失败就判该场景失败。
 */
static int scen_run(int mode, scene *sc)
{
    const char *why0 = 0;
    uint32_t    ncount = PROBE_COUNT_BASE;
    int         tries = 0;
    int         r = R_OK;

    for (;;) {
        *sc = scene_zero;
        sc->count = (mode >= 0) ? ncount : 0u;
        tries++;
        r = one_attempt(mode, ncount, sc);
        if (r == R_OK)
            break;
        if (why0 == 0)
            why0 = why_str(r);
        if ((tries >= 2) || !retryable(r))
            break;
        ncount = (r == R_EARLY_DONE) ? PROBE_COUNT_RETRY : PROBE_COUNT_BASE;
    }
    if (tries > 1) {
        sc->retried = 1u;
        sc->first_fail = why0;
    }
    sc->ok = (r == R_OK) ? 1u : 0u;
    sc->reason = why_str(r);
    return sc->ok ? 0 : 1;
}

static void report_scene(const char *name, const scene *sc)
{
    lbegin("SCEN ");
    core_out_str(&s_out, name);
    ldec(" n=", sc->samples);
    ldec(" min=", sc->min);
    ldec(" max=", sc->max);
    ldec(" first=", sc->first_frame);
    ldec(" med_lo=", sc->med_lo);
    ldec(" med_hi=", sc->med_hi);
    lend();

    lbegin("SCEN ");
    core_out_str(&s_out, name);
    ldec(" bad beats=", sc->beats_bad);
    ldec(" end=", sc->end_bad);
    ldec(" low=", sc->low_bad);
    lhex(" zero=", sc->zero);
    lhex(" under=", sc->under);
    lhex(" resperr=", sc->resperr);
    ldec(" skip=", sc->skip);
    lend();

    if (sc->count != 0u) {
        lbegin("SCEN ");
        core_out_str(&s_out, name);
        lhex(" probe status=", sc->p_status);
        lhex(" txdone=", sc->p_txdone);
        lhex(" count=", sc->count);
        lhex(" errs=", sc->p_errs);
        lhex(" cycles=", sc->p_cycles);
        lend();
    }

    lbegin("SCEN ");
    core_out_str(&s_out, name);
    ldec(" scan txn=", sc->d_txn);
    ldec(" beat=", sc->d_beat);
    ldec(" max_first=", sc->d_max_first);
    ldec(" max_full=", sc->d_max_full);
    ldec(" max_gap=", sc->d_max_gap);
    lhex(" status=", sc->d_status);
    lend();

    lbegin("SCEN ");
    core_out_str(&s_out, name);
    lhex(" guard status=", sc->g_status);
    lhex(" hits=", sc->g_hits);
    lhex(" max_ar=", sc->g_max_ar);
    lhex(" max_aw=", sc->g_max_aw);
    lend();

    if (sc->retried != 0u) {
        lbegin("SCEN ");
        core_out_str(&s_out, name);
        core_out_str(&s_out, " retry first_fail=");
        core_out_str(&s_out, sc->first_fail);
        lend();
    }

    if (sc->ok == 0u) {
        /* 失败先留现场：ABORT 之前保存的那一组数 */
        lbegin("SCEN ");
        core_out_str(&s_out, name);
        core_out_str(&s_out, " scene aborted=");
        core_out_u32(&s_out, sc->aborted);
        lhex(" after=", sc->p_status_after);
        lhex(" pstatus=", sc->p_status);
        lhex(" ptxdone=", sc->p_txdone);
        lhex(" snap_frame=", sc->f_frame);
        ldec(" snap_min=", sc->f_min);
        lhex(" snap_zero=", sc->f_zero);
        lhex(" snap_under=", sc->f_under);
        lhex(" snap_beats=", sc->f_beats);
        lhex(" snap_end=", sc->f_end);
        lend();
    }

    lbegin("SCEN ");
    core_out_str(&s_out, name);
    if (sc->ok != 0u) {
        core_out_str(&s_out, " ok");
    } else {
        core_out_str(&s_out, " FAIL why=");
        core_out_str(&s_out, sc->reason);
    }
    lend();
}

int main(void)
{
    static const char *names[3] = { "idle", "read", "write" };
    static const int   modes[3] = { -1, 0, (int)P_MODE_WRITE };
    scene sc[3];
    int   i;

    board_init();
    board_puts("\r\ndisplayFifoProbe\r\n");

    if (ident_check() != 0) {
        /* 比特流不对就不测：接着跑只会得到一堆没有意义的数 */
        board_puts("fail 1\r\ndone\r\n");
        return 0;
    }

    fill_pattern();
    board_present(0);                   /* 让 FB0 成为扫描的源，并打开 EN|SRC */

    lbegin("SETUP fb0=");
    core_out_hex32(&s_out, (uint32_t)(uintptr_t)board_fb(0));
    lhex(" front=", drd(D_FRONT));
    lhex(" ctrl=", drd(D_CTRL));
    lend();

    for (i = 0; i < 3; i++)
        fails += scen_run(modes[i], &sc[i]);

    /* 采样期一个字节都不打印，三组跑完再统一出结果 */
    for (i = 0; i < 3; i++)
        report_scene(names[i], &sc[i]);

    lbegin("fail ");
    core_out_u32(&s_out, (uint32_t)fails);
    lend();
    board_puts("done\r\n");
    return 0;
}
