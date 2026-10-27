/******************************************************************************
* @file rtos_irq.c: FreeRTOS 外部中断桥（SW10）
*
* @brief  本内核版本（V202212.01 的 GCC/RISC-V 端口）的 trap 分派只认 machine
*         timer（tick）；**其余异步中断一律进 portASM.S 里 .weak 的
*         freertos_risc_v_application_interrupt_handler，其缺省实现是死循环**。
*         （freertosDemo makefile 里 -DportasmHANDLE_INTERRUPT=... 是旧版端口的
*         残留，本版本 portASM.S 不引用该宏。）所以集成层在这里提供强符号覆盖：
*         machine external -> bsp 的 PLIC claim 循环 + 帧信号量；
*         其它未预期中断 -> 打一行并停下（可见的失败，比静默转下去干净）。
*
*         不调 board_irq_init 的裸机路径仍走 trap()，两者互不干扰。
******************************************************************************/

#include "FreeRTOS.h"
#include "semphr.h"

#include "board.h"

#include "bsp.h"            /* sdk：csr_read */
#include "riscv.h"

#include "rtos_irq.h"

void freertos_risc_v_trap_handler(void);   /* 内核端口入口（portASM.S） */

static SemaphoreHandle_t s_frame_sem;

int rtos_irq_init(void)
{
    s_frame_sem = xSemaphoreCreateCounting(4u, 0u);
    if (s_frame_sem == NULL)
        return -1;
    /* PLIC 阈值/优先级/使能、场起始源使能与 mtvec=trap_entry 都在 bsp 里做；
       mtvec 随后由 rtos_trap_install 换成 FreeRTOS 端口入口。 */
    return board_irq_init();
}

void rtos_trap_install(void)
{
    csr_write(mtvec, freertos_risc_v_trap_handler);
}

/* 覆盖 portASM.S 的 .weak 缺省（死循环）。非 timer 异步中断都进这里。
 *
 * 🔴 tick 源（2026-10-26 板上实测）：本 SoC 构型的 mtimecmp 写入丢失、
 * machine timer 中断从不触发（裸机软件只用过 mtime 计数，从未暴露）。
 * tick 由此处在每个场起始事件上模拟 portASM 的 tick 路径：
 * xTaskIncrementTick -> （有就绪变化时）vTaskSwitchContext，返回后
 * portASM 的 processed_source 会按切换好的上下文恢复——与端口自己的
 * timer 分支同一套调用约定（trap 入口硬件清 MIE）。
 *
 * 调度器没起来之前（main 装 trap 到 vTaskStartScheduler 之间的窗口、
 * 以及内核初始化期间）只派发不碰内核：此时任务链表尚未初始化，
 * 提前调 xTaskIncrementTick 会把它们弄坏（首帧中断撞进窗口的实测）。 */
void freertos_risc_v_application_interrupt_handler(void)
{
    const uint32_t cause = (uint32_t)csr_read(mcause);

    if (cause == (0x80000000u | 11u)) {    /* 11 == machine external（PLIC） */
        const uint32_t before = board_irq_frame_events();

        board_irq_dispatch_external();

        if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
            const uint32_t frames = board_irq_frame_events() - before;
            BaseType_t    wake = pdFALSE;

            for (uint32_t i = 0u; i < frames; i++) {
                xSemaphoreGiveFromISR(s_frame_sem, &wake);
                if (xTaskIncrementTick() != pdFALSE)
                    vTaskSwitchContext();
            }
            portYIELD_FROM_ISR(wake);
        }
        return;
    }
    /* 未预期的中断：可见地停下（本 SoC 只该有 timer 与 external 两种异步） */
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}

int rtos_wait_frame(uint32_t ms)
{
    return (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(ms)) == pdTRUE) ? 0 : -1;
}

/* 🔴 接管内核的定时器初始化（覆盖 port.c 的 __weak 版本）。
 *
 * 板上实测（2026-10-26）：本 SoC 构型的 mtimecmp「写生效、读恒 0」，且硬件
 * timer 中断的实 rate 不是 configTICK_RATE_HZ 期望的 60Hz（实测 ~1250/s，
 * 机制未明——portASM 的 cmp 递增模式在这个 CLINT 上行为异常）。与其猜硬件，
 * 不如让硬件 timer 分支彻底休眠：cmp 写到极远未来（mtime + 2^31），MTIP
 * 永不置位；tick 的唯一来源是上面的场中断桥——每场一 tick，精确 60Hz，
 * 与 configTICK_RATE_HZ 一致。产品线若要毫秒级 tick，在 SoC IP 层修 CLINT
 * 并重综合后恢复 port.c 的弱实现即可。
 *
 * 注意：写法沿用 clint_setCmp 的安全次序（高字 0xFFFFFFFF 先行，低字后写，
 * 再写高字），低字取「mtime 低 32 位 + 0x7FFFFFFF」的截断值。 */
void vPortSetupTimerInterrupt(void)
{
    volatile uint32_t * const cmp = (volatile uint32_t *)0xf8b04000u;
    const volatile uint32_t * const mt = (const volatile uint32_t *)0xf8bbff8u;
    const uint32_t lo = mt[0] + 0x7FFFFFFFu;
    uint32_t       hi = mt[1];

    if (lo < mt[0])             /* 低字回绕：进位到高字，cmp 必须仍在未来 */
        hi++;
    cmp[1] = 0xFFFFFFFFu;
    cmp[0] = lo;
    cmp[1] = hi;
}
