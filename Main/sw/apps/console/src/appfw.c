/******************************************************************************
* @file appfw.c: 应用框架的运行侧（SW10）
*
* @brief  三个任务撑起游戏机形态：
*           runner（优先级 3，64K 栈）  启动共享运行时，然后逐帧驱动当前应用
*           inputd（优先级 4，2K 栈）   每场采样一次 board_keys（独占：UART 收包
*                                       与 GPIO 快照只有这一个调用方），合并成
*                                       「held|自上次取走以来的按下沿」
*           monitor（优先级 1，4K 栈）  10s 一行存活证据（tick/堆/任务数），
*                                       只在菜单时说话，不搅游戏的判据行
*
*         tick=场频（60Hz，见 FreeRTOSConfig.h 文件头的板上实测记录）。
*         HOME = 三模式键同按 8 帧（armed/re-arm 语义同 stg_chord），由 runner
*         检测并切回菜单；应用切换走 appfw_request，在帧边界生效。
*         日志纪律与裸机一致：全部行进 logq，每帧只取 board_tx_room() 那么多。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"

#include "core/format.h"
#include "core/logq.h"

#include "gfx/gfx.h"

#include "stg_modes.h"
#include "r4.h"

#include "rtos_irq.h"

#include "chord.h"

#include "appfw.h"

/* --- 输入（inputd 独占 board_keys，runner/appfw_input_take 消费） --------- */

static volatile uint32_t s_in_pending;   /* 自上次取走以来的按键并集 */
static volatile uint32_t s_in_held;      /* 最近一次采样的按住位图 */

static void input_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t k;

        vTaskDelay(1);     /* 一场一采样：tick=场频（60Hz），与裸机每帧读一次同节奏 */
        /* board_keys 内部有静态状态（UART 收包解析），必须独占调用；
           临界区只包采样本身，几条 MMIO 读，不挡帧中断。 */
        taskENTER_CRITICAL();
        k = board_keys();
        taskEXIT_CRITICAL();
        s_in_pending |= k;
        s_in_held = k;
    }
}

uint32_t appfw_input_take(void)
{
    uint32_t r;

    taskENTER_CRITICAL();
    r = s_in_pending;
    s_in_pending = s_in_held;    /* 按住的位保留给下一帧，沿位已被取走 */
    taskEXIT_CRITICAL();
    return r;
}

/* 最近一次采样的按住位图（电平，不消耗沿）。r4 的条带内键采样经
   r4_set_key_source 注入它——r4 在框架外直读 board_keys 会抢走 UART
   解析的字节，破坏 inputd 的独占约定。 */
uint32_t appfw_input_held(void)
{
    return s_in_held;
}

/* --- 日志队列与泵（同裸机 main.c 的 LOGQ 纪律） --------------------------- */

#define LOGQ_BYTES   8192u
#define LOG_TAKE_MAX 256u

static logq  s_logq;
static char  s_logq_mem[LOGQ_BYTES];
static char  s_log_out[LOG_TAKE_MAX + 1u];

void appfw_log(const char *s)
{
    (void)logq_put(&s_logq, s);
}

/* monitor 的发布缓冲：monitor 任务不直接进 logq（logq 是单写者设计，
   runner 侧的 telemetry/put 与 pump 同任务；monitor 并发 put 会把 head/len
   写坏，2026-10-28 板上实测队列积压 5KB 永不排空、rtos tick 行全部丢失）。
   monitor 只发布到这里，log_pump 在 runner 上下文转投进队列。 */
static char     s_mon_line[128];
static volatile uint32_t s_mon_len;
static volatile uint32_t s_mon_wakes;    /* 诊断：monitor 循环实际醒来几次 */

static void log_pump(void)
{
    const uint32_t room = board_tx_room();
    uint32_t       n = (room < LOG_TAKE_MAX) ? room : LOG_TAKE_MAX;

    if (s_mon_len != 0u) {
        /* 转投进 logq 只看队列自己的空位（8KB），与 UART 这一帧的房间无关；
           队列满就留到下一帧。 */
        if (logq_put(&s_logq, s_mon_line) == 0)
            s_mon_len = 0u;
    }
    if (n == 0u || logq_used(&s_logq) == 0u)
        return;
    n = logq_take(&s_logq, s_log_out, n);
    board_puts(s_log_out);
}

/* --- HOME 和弦：三模式键同按 8 帧（逻辑在 chord.[ch]，host 单测直接编它） - */

#define HOME_COMBO (BOARD_KEY_MODE1 | BOARD_KEY_MODE2 | BOARD_KEY_MODE3)
#define HOME_HOLD  8u

static chord s_home;

/* --- 注册表（shell.c 定义数组；这里只取） --------------------------------- */

const appfw_app *const *appfw_apps(uint32_t *count)
{
    extern const appfw_app *const g_appfw_registry[];
    extern const uint32_t         g_appfw_registry_n;

    *count = g_appfw_registry_n;
    return g_appfw_registry;
}

const appfw_app *appfw_app_at(uint32_t index)
{
    uint32_t n;

    const appfw_app *const *apps = appfw_apps(&n);
    if (index >= n)
        return NULL;
    return apps[index];
}

const appfw_app *appfw_find(const char *name)
{
    uint32_t n, i;

    const appfw_app *const *apps = appfw_apps(&n);
    for (i = 0u; i < n; i++)
        if (strcmp(apps[i]->name, name) == 0)
            return apps[i];
    return NULL;
}

/* --- 运行器 --------------------------------------------------------------- */

static stg_runtime      s_rt;
static appfw_ctx        s_ctx;
static const appfw_app *s_active;
static const appfw_app *s_request;
static uint32_t         s_app_frames;

int appfw_in_shell(void)
{
    return s_active != NULL && strcmp(s_active->name, "menu") == 0;
}

void appfw_request(const appfw_app *app)
{
    s_request = app;
}

static void switch_to(const appfw_app *next)
{
    char     buf[96];
    core_out o;

    if (s_active != NULL) {
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "console leave ");
        core_out_str(&o, s_active->name);
        core_out_str(&o, "\r\n");
        appfw_log(buf);
        s_active->leave(&s_ctx);
    }
    exec_sw_reset(&s_rt.es);
    s_active     = next;
    s_app_frames = 0u;
    s_request    = NULL;
    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "console enter ");
    core_out_str(&o, next->name);
    core_out_str(&o, "\r\n");
    appfw_log(buf);
    if (next->enter(&s_ctx) != 0)
        appfw_log("console enter err\r\n");
}

/* 帧时间的一秒窗口（满一秒结算一次 swap 数）由各适配器自持：菜单与游戏的
   HUD 口径一致，但不把统计塞进框架——应用才知道自己的一帧从哪到哪。 */

/* runner 迭代的忙耗时（循环体全程，不含被调度出去的时间）。应用每秒取走
   一次做「迭代周期 vs 忙耗时」对照：差值就是 runner 被阻塞/抢占的时间。 */
static uint32_t s_itr_sum, s_itr_n;

void appfw_loop_cycles(uint32_t *sum, uint32_t *n)
{
    *sum     = s_itr_sum;
    *n       = s_itr_n;
    s_itr_sum = 0u;
    s_itr_n   = 0u;
}

static void runner_task(void *arg)
{
    const appfw_app *shell;

    (void)arg;
    if (stg_runtime_boot(&s_rt) != 0) {
        board_puts("console boot err\r\n");
        for (;;)
            vTaskDelay(portMAX_DELAY);
    }
    stg_runtime_set_active(&s_rt);
    s_rt.backend = 1;           /* 产品默认 HW/DDR；Tab 在游戏里可切 SW 对照 */

    s_ctx.rt     = &s_rt;
    s_ctx.seq    = 0u;
    s_ctx.shown  = -1;
    s_ctx.log    = &s_logq;     /* 队列本体在 appfw_start 里已初始化 */

    stg_modes_set_log(&s_logq);
    r4_set_log(&s_logq);
    stg_runtime_r4_install_hooks();
    r4_set_key_source(appfw_input_held);   /* r4 条带内采样走 inputd 的按住位图 */

    /* TEX 热集先试装一次：失败不致命（HW/TEX 后端退回 HW/DDR），打出定位码 */
    {
        const int tex_rc = stg_runtime_tex_load(&s_rt);

        if (tex_rc != 0) {
            char     buf[64];
            core_out o;

            core_out_init(&o, buf, (uint32_t)sizeof buf);
            core_out_str(&o, "console tex err ");
            core_out_u32(&o, (uint32_t)(-tex_rc));
            core_out_str(&o, " HW/TEX disabled\r\n");
            appfw_log(buf);
        }
    }

    shell = appfw_find("menu");
    chord_init(&s_home, HOME_COMBO, HOME_HOLD);
    switch_to(shell);

    for (;;) {
        const uint32_t t0   = board_cycles();
        const uint32_t keys = appfw_input_take();
        const uint32_t ev   = chord_step(&s_home, keys);

        if ((ev & CHORD_EV_FIRE) != 0u && s_active != shell) {
            switch_to(shell);
        } else if (s_request != NULL && s_request != s_active) {
            switch_to(s_request);
        } else {
            s_active->frame(&s_ctx, keys, s_app_frames);
            s_app_frames++;
        }
        log_pump();
        s_itr_sum += board_cycles() - t0;
        s_itr_n++;
    }
}

/* --- monitor：10s 一行存活证据（只在菜单时说话） -------------------------- */

static void monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        s_mon_wakes++;
        if (appfw_in_shell() && s_mon_len == 0u) {
            char     buf[128];
            core_out o;
            uint32_t n;

            core_out_init(&o, buf, (uint32_t)sizeof buf);
            core_out_str(&o, "rtos tick ");
            core_out_u32(&o, (uint32_t)xTaskGetTickCount());
            core_out_str(&o, " heap ");
            core_out_u32(&o, (uint32_t)xPortGetFreeHeapSize());
            core_out_str(&o, " tasks ");
            core_out_u32(&o, (uint32_t)uxTaskGetNumberOfTasks());
            core_out_str(&o, " irqA ");
            core_out_u32(&o, board_irq_frame_events());
            core_out_str(&o, "\r\n");
            /* 只发布，不直接进 logq（单写者约定，见 s_mon_line 处注释）。
               strlen 可安全截断：core_out_init 保证 buf 里有 '\0'。 */
            n = 0u;
            while (buf[n] != '\0')
                n++;
            if (n > (uint32_t)sizeof s_mon_line - 1u)
                n = (uint32_t)sizeof s_mon_line - 1u;
            for (uint32_t i = 0u; i < n; i++)
                s_mon_line[i] = buf[i];
            s_mon_line[n] = '\0';
            s_mon_len = n;               /* 单字发布；runner 消费后清零 */
        }
    }
}

/* --- 入口（main.c 调） ----------------------------------------------------- */

void appfw_start(void)
{
    logq_init(&s_logq, s_logq_mem, LOGQ_BYTES);

    /* 优先级契约：runner（3）整循环从不阻塞——present 等场是寄存器忙轮询
       （board.c 与裸机共用，不走帧信号量）——所以任何优先级 ≤3 的任务都会
       被永久饿死（2026-10-28 板上实测：monitor 在 60s 菜单驻留里零唤醒）。
       inputd(4) 每场醒来靠抢占；monitor 必须同为 4：醒来即抢占、干完微秒级
       工作立刻 Delay。runner 若改为在 present 上阻塞帧信号量，可再降回。 */
    if (xTaskCreate(input_task, "inputd", 512, NULL, 4, NULL) != pdPASS
        || xTaskCreate(runner_task, "runner", 16384, NULL, 3, NULL) != pdPASS
        || xTaskCreate(monitor_task, "monitor", 1024, NULL, 4, NULL) != pdPASS) {
        board_puts("console task create err\r\n");
        for (;;) { }
    }
}
