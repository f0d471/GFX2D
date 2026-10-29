/* FreeRTOS RISC-V port: CLINT owns the 1kHz kernel tick; PLIC owns display
 * and engine events. A display interrupt never advances the kernel clock. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "board.h"
#include "bsp.h"
#include "riscv.h"
#include "rtos_irq.h"

void freertos_risc_v_trap_handler(void);
static SemaphoreHandle_t s_frame_sem;
static SemaphoreHandle_t s_engine_sem;

int rtos_irq_init(void)
{
    s_frame_sem = xSemaphoreCreateCounting(4u, 0u);
    s_engine_sem = xSemaphoreCreateCounting(4u, 0u);
    if (s_frame_sem == NULL || s_engine_sem == NULL)
        return -1;
    return board_irq_init();
}

void rtos_trap_install(void)
{
    csr_write(mtvec, freertos_risc_v_trap_handler);
}

/* The vendor port's timer branch calls xTaskIncrementTick itself. Its weak
 * application handler is only for non-timer interrupts. */
void freertos_risc_v_application_interrupt_handler(void)
{
    const uint32_t cause = (uint32_t)csr_read(mcause);
    if (cause == (0x80000000u | 11u)) {
        const uint32_t before = board_irq_frame_events();
        const uint32_t engine_before = board_irq_engine_events();
        board_irq_dispatch_external();
        if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
            uint32_t frames = board_irq_frame_events() - before;
            uint32_t engines = board_irq_engine_events() - engine_before;
            BaseType_t wake = pdFALSE;
            while (frames-- != 0u)
                xSemaphoreGiveFromISR(s_frame_sem, &wake);
            while (engines-- != 0u)
                xSemaphoreGiveFromISR(s_engine_sem, &wake);
            portYIELD_FROM_ISR(wake);
        }
        return;
    }
    taskDISABLE_INTERRUPTS();
    board_puts("console fatal interrupt\r\n");
    for (;;) { }
}

int rtos_wait_frame(uint32_t ms)
{
    return xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(ms)) == pdTRUE ? 0 : -1;
}

int rtos_wait_engine(uint32_t ms)
{
    return xSemaphoreTake(s_engine_sem, pdMS_TO_TICKS(ms)) == pdTRUE ? 0 : -1;
}

/* Do not override vPortSetupTimerInterrupt: use checked BSP addresses and
 * the normal port timer implementation. cmp readback is not a capability test. */
