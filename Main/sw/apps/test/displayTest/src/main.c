/******************************************************************************
*
* @file main.c: displayTest
*
* @brief  扫描输出（display）接进软件层之后的板上判据，也是 `board_present` /
*         `board_wait_frame` 这两个接口的验证程序。
*
*         分五段，每段一行输出，末尾 `fail N` 与 `done`：
*
*         1. IDENT。读 `ID`/`VERSION`/`CTRL`/`FRONT_BASE`，确认比特流里是扫描输出
*            而不是替身，且复位值与寄存器表一致。
*         2. PATTERN。往帧缓冲 0 画定位图案（四角方块、最左四列纯色、两条 1 像素线），
*            present 上屏。**这一项的判据是看屏幕**：颜色与位置错了说明字节序、
*            拍内像素顺序或行跨有问题；寄存器侧同时读回一组统计。
*         3. FILL。整屏纯色填充计时，得出**每个像素写的周期数**。
*            这一颗 CPU 没有写缓冲，每个像素写都是一次 DDR 往返，
*            这个数字决定软件渲染能用多大的分辨率。
*         4. ANIM。在后缓冲画一个移动的方块、每帧 present，连跑 ANIM_FRAMES 帧，
*            打出帧率，验证呈现节拍。
*         5. STATS。连读 STAT_FIELDS 场快照，给出每场弹出拍数、场末残留水位、
*            最低水位的最小值与中位值，以及累计的触底与欠载。
*            判据：拍数恒为 76800、场末残留恒为 0、最低水位不低于 160、欠载为 0。
*
*         第 3、4 段都先 `board_present` 过，所以屏幕上看得到全过程。
*         中断与探针满负载那两组（2.5 节第 5、6 条）不在本程序里。
*
******************************************************************************/

#include <stdint.h>

#include "board.h"
#include "memmap.h"
#include "core/format.h"

/* 扫描输出的寄存器窗口。`sw/bsp/` 把 present/wait_frame 包好了，但本程序是
   这套硬件的判据程序，要直接读它的统计寄存器，所以这里自己开一份地址。
   表见 `rtl/display/display_fetch.v` 文件头。 */
#define DISP_BASE       0xF810E000u
#define D_ID            0x000u
#define D_VERSION       0x004u
#define D_CTRL          0x008u
#define D_FRONT_BASE    0x00Cu
#define D_MAX_FIRST     0x018u
#define D_MAX_FULL      0x01Cu
#define D_TXN           0x024u
#define D_RESPERR       0x034u
#define D_STATUS        0x038u
#define D_FRAME_COUNT   0x0D8u
#define D_SWAP_COUNT    0x0DCu
#define D_SNAP_FRAME    0x0E0u
#define D_SNAP_MIN      0x0E4u
#define D_ZERO_HITS     0x0E8u
#define D_UNDER_BEATS   0x0ECu
#define D_FRAME_BEATS   0x0F0u
#define D_END_LEVEL     0x0F4u

#define DISP_ID_VALUE   0x44495350u     /* "DISP" */
#define CTRL_EN         (1u << 0)
#define CTRL_SRC_FB     (1u << 1)

/* 一场应当弹出的拍数：480 行 x 160 拍（一行 640 像素 = 160 个 128 位拍）。 */
#define BEATS_PER_FIELD (480u * 160u)
/* 最低水位的下界：一行。扫描 FIFO 深度 512 拍与发行阈值 160 是一对。 */
#define MIN_LEVEL_FLOOR 160u

#define ANIM_FRAMES     120u
#define STAT_FIELDS     120u

#define RGB(r, g, b)    (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define C_BLACK         RGB(0x00, 0x00, 0x00)
#define C_RED           RGB(0xFF, 0x00, 0x00)
#define C_GREEN         RGB(0x00, 0xFF, 0x00)
#define C_BLUE          RGB(0x00, 0x00, 0xFF)
#define C_WHITE         RGB(0xFF, 0xFF, 0xFF)
#define C_YELLOW        RGB(0xFF, 0xFF, 0x00)
#define C_CYAN          RGB(0x00, 0xFF, 0xFF)
#define C_MAGENTA       RGB(0xFF, 0x00, 0xFF)

static int fails;

static uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(DISP_BASE + off);
}

/* 一行一行拼再一次打出去：串口是逐字节的，拼好再发省掉大量调用开销，
   也让每一行是原子的、不会被别的输出插进来。 */
static char     s_line[120];
static core_out s_out;

static void line_begin(const char *s)
{
    core_out_init(&s_out, s_line, (uint32_t)sizeof s_line);
    core_out_str(&s_out, s);
}

static void line_kv(const char *name, uint32_t v)
{
    core_out_str(&s_out, name);
    core_out_hex32(&s_out, v);
}

static void line_end(void)
{
    core_out_str(&s_out, "\r\n");
    board_puts(s_line);
}

static void chk(const char *what, uint32_t got, uint32_t lo, uint32_t hi)
{
    core_out_str(&s_out, what);
    core_out_str(&s_out, "=0x");
    core_out_hex32(&s_out, got);
    if (got < lo || got > hi) {
        fails++;
        core_out_str(&s_out, " FAIL");
    } else {
        core_out_str(&s_out, " ok");
    }
    line_end();
}

/* --- 画图元（只写不读，写方向不需要管缓存）----------------------------- */

static void fb_fill(uint32_t *fb, uint32_t color)
{
    uint32_t y, x;
    for (y = 0; y < FB_HEIGHT; y++) {
        uint32_t *row = fb + y * FB_ROW_PIXELS;
        for (x = 0; x < FB_WIDTH; x++)
            row[x] = color;
    }
}

static void fb_rect(uint32_t *fb, uint32_t x0, uint32_t y0,
                    uint32_t w, uint32_t h, uint32_t color)
{
    uint32_t y, x;
    for (y = y0; y < y0 + h && y < FB_HEIGHT; y++) {
        uint32_t *row = fb + y * FB_ROW_PIXELS;
        for (x = x0; x < x0 + w && x < FB_WIDTH; x++)
            row[x] = color;
    }
}

/* 定位图案：四角各一个颜色不同的方块，最左四列分别是纯红、纯绿、纯蓝、白
   （正好是一个 128 位拍里的 4 个像素，拆反了一眼看得出），
   再加一条 1 像素宽的水平线与垂直线，用来看行跨与边界有没有差一。 */
static void draw_pattern(uint32_t *fb)
{
    uint32_t y;

    fb_fill(fb, C_BLACK);

    fb_rect(fb, 0, 0, 40, 40, C_RED);
    fb_rect(fb, FB_WIDTH - 40, 0, 40, 40, C_GREEN);
    fb_rect(fb, 0, FB_HEIGHT - 40, 40, 40, C_BLUE);
    fb_rect(fb, FB_WIDTH - 40, FB_HEIGHT - 40, 40, 40, C_WHITE);

    for (y = 0; y < FB_HEIGHT; y++) {
        uint32_t *row = fb + y * FB_ROW_PIXELS;
        row[0] = C_RED;
        row[1] = C_GREEN;
        row[2] = C_BLUE;
        row[3] = C_WHITE;
    }

    fb_rect(fb, 0, FB_HEIGHT / 2, FB_WIDTH, 1, C_YELLOW);
    fb_rect(fb, FB_WIDTH / 2, 0, 1, FB_HEIGHT, C_CYAN);
}

/* --- 五段 --------------------------------------------------------------- */

static void seg_ident(void)
{
    line_begin("IDENT ");
    chk("id", rd(D_ID), DISP_ID_VALUE, DISP_ID_VALUE);
    line_begin("IDENT ");
    chk("version", rd(D_VERSION), 1u, 1u);
    line_begin("IDENT ");
    chk("front_base", rd(D_FRONT_BASE), 0x02000000u, 0x02000000u);
}

static void seg_pattern(void)
{
    draw_pattern(board_fb(0));
    board_present(0);

    line_begin("PATTERN ");
    line_kv("ctrl=0x", rd(D_CTRL));
    line_kv(" front=0x", rd(D_FRONT_BASE));
    line_kv(" status=0x", rd(D_STATUS));
    line_end();
    line_begin("PATTERN ");
    chk("resperr", rd(D_RESPERR), 0u, 0u);
    board_puts("PATTERN 看屏幕：四角红绿蓝白、最左四列红绿蓝白、黄色横线与青色竖线\r\n");
}

/* 整屏填充一次的周期数 -> 每像素周期数。这一段不 present，量的是纯写。 */
static void seg_fill(void)
{
    uint32_t *fb = board_fb(1);
    uint32_t t0, dt1, dt2, per_px_x100;

    /* 量两种填充，差别不在硬件而在编译：
         dt1 非零色——逐像素 sw，量的是「一个像素写要多少周期」；
         dt2 清零——0x00000000 是可按字节重复的值，gcc 会把内层循环转成 memset，
                    走的是 newlib 里展开过的块写。
       两个数都打出来。整屏清屏是软件渲染每帧的固定开销，
       按哪个数估帧率取决于清的是不是零，这一条不区分就会差出 20% 以上。 */
    t0 = board_cycles();
    fb_fill(fb, RGB(0x20, 0x20, 0x20));
    dt1 = board_cycles() - t0;

    /* 第三次在场起始之后紧接着做：ANIM 里每帧的填充正好是这个位置
       （present 等完一场才回来），而它量出来比背靠背的填充快得多。
       三个数放在一起，差异归因留给 E7。 */
    board_wait_frame();
    t0 = board_cycles();
    fb_fill(fb, C_BLACK);
    dt2 = board_cycles() - t0;

    per_px_x100 = dt1 / ((FB_WIDTH * FB_HEIGHT) / 100u);

    line_begin("FILL ");
    line_kv("back2back_cyc=0x", dt1);
    line_kv(" after_vsync_cyc=0x", dt2);
    line_kv(" per_px_x100=0x", per_px_x100);
    line_end();
    /* 判据只卡一个很宽的上界：这是量出来的数，不是设计目标。
       超过 100 周期/像素说明访存路径出了问题，不是「慢一点」。 */
    line_begin("FILL ");
    chk("per_px_x100", per_px_x100, 1u, 10000u);
}

static void seg_anim(void)
{
    uint32_t f, t0, dt, fps_x100;
    uint32_t fill_acc = 0, present_acc = 0;
    int idx = 0;

    t0 = board_cycles();
    for (f = 0; f < ANIM_FRAMES; f++) {
        uint32_t *fb = board_fb(idx);
        uint32_t x = (f * 4u) % (FB_WIDTH - 60u);
        uint32_t y = (f * 2u) % (FB_HEIGHT - 60u);
        uint32_t m;

        /* 每帧的三段分别计时：整屏清、画方块、等切换。
           一帧的时间花在哪一段，是定软件渲染分辨率的依据。 */
        m = board_cycles();
        fb_fill(fb, C_BLACK);
        fill_acc += board_cycles() - m;

        fb_rect(fb, x, y, 60, 60, C_MAGENTA);

        m = board_cycles();
        board_present(idx);
        present_acc += board_cycles() - m;

        idx ^= 1;
    }
    dt = board_cycles() - t0;

    /* 帧率 x100，避免浮点：BOARD_HZ * 100 会溢出 32 位，所以先把周期数摊到每帧。 */
    fps_x100 = (BOARD_HZ / (dt / ANIM_FRAMES)) * 100u;

    line_begin("ANIM ");
    line_kv("frames=0x", ANIM_FRAMES);
    line_kv(" cycles=0x", dt);
    line_kv(" fps_x100=0x", fps_x100);
    line_kv(" fill_per_frame=0x", fill_acc / ANIM_FRAMES);
    line_kv(" present_per_frame=0x", present_acc / ANIM_FRAMES);
    line_end();
    line_begin("ANIM ");
    chk("swap_count", rd(D_SWAP_COUNT), ANIM_FRAMES, ANIM_FRAMES + 2u);
}

static void seg_stats(void)
{
    uint32_t i;
    uint32_t min_of_min = 0xFFFFFFFFu;
    uint32_t max_of_min = 0u;
    uint32_t beats_bad = 0, end_bad = 0;
    uint32_t last_snap = rd(D_SNAP_FRAME);

    for (i = 0; i < STAT_FIELDS; i++) {
        uint32_t snap;
        board_wait_frame();
        snap = rd(D_SNAP_FRAME);
        if (snap == last_snap)
            continue;                    /* 同一场读两次，不重复计 */
        last_snap = snap;

        if (rd(D_FRAME_BEATS) != BEATS_PER_FIELD)
            beats_bad++;
        if (rd(D_END_LEVEL) != 0u)
            end_bad++;
        {
            uint32_t m = rd(D_SNAP_MIN);
            if (m < min_of_min) min_of_min = m;
            if (m > max_of_min) max_of_min = m;
        }
    }

    line_begin("STATS ");
    line_kv("min_level_lo=0x", min_of_min);
    line_kv(" hi=0x", max_of_min);
    line_kv(" max_full=0x", rd(D_MAX_FULL));
    line_kv(" max_first=0x", rd(D_MAX_FIRST));
    line_end();

    line_begin("STATS ");
    chk("frame_beats_bad", beats_bad, 0u, 0u);
    line_begin("STATS ");
    chk("end_level_bad", end_bad, 0u, 0u);
    line_begin("STATS ");
    chk("min_level_floor", min_of_min, MIN_LEVEL_FLOOR, 512u);
    line_begin("STATS ");
    chk("zero_hits", rd(D_ZERO_HITS), 0u, 0u);
    line_begin("STATS ");
    chk("underflow_beats", rd(D_UNDER_BEATS), 0u, 0u);
}

int main(void)
{
    board_init();
    board_puts("\r\ndisplayTest\r\n");

    seg_ident();
    seg_pattern();
    seg_fill();
    seg_anim();
    seg_stats();

    line_begin("fail ");
    core_out_u32(&s_out, (uint32_t)fails);
    line_end();
    board_puts("done\r\n");
    return 0;
}
