/******************************************************************************
* @file main.c: console 入口（SW10）
*
* @brief  游戏机前端：FreeRTOS 起三个任务（runner/inputd/monitor，见 appfw.c），
*         场起始中断经 PLIC -> FreeRTOS 端口 -> 项目派发（sw/rtos 的桥），
*         tick 走 CLINT mtime。启动次序对齐厂商 demo：PLIC 与源使能 ->
*         mtvec 指到 FreeRTOS 端口入口 -> vTaskStartScheduler。
*
*         板上判据的行序（tools/check/check_console_board.py）：
*           console boot / pak len .. crc .. / console enter menu / menu crc .. /
*           console enter stg| r4 | about / console leave .. / rtos tick ..
******************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"

#include "core/crc32.h"

#include "rtos_irq.h"

#include "appfw.h"

int main(void)
{
    board_init();
    crc32_init();

    if (rtos_irq_init() != 0) {
        board_puts("console irq init err\r\n");
        for (;;) { }
    }
    board_puts("console boot\r\n");

    appfw_start();

    rtos_trap_install();
    vTaskStartScheduler();

    for (;;) { }
}

/* --- 内核钩子（FreeRTOSConfig 引的三个） ---------------------------------- */

void vApplicationIdleHook(void)
{
    /* 不阻塞、不做活：只证明调度器在转（monitor 的 rtos tick 行是证据） */
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    board_puts("console malloc failed\r\n");
    for (;;) { }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    taskDISABLE_INTERRUPTS();
    board_puts("console stack overflow\r\n");
    for (;;) {
        (void)name;
    }
}

void vAssertCalled(const char *file, int line)
{
    taskDISABLE_INTERRUPTS();
    board_puts("console assert\r\n");
    for (;;) {
        (void)file;
        (void)line;
    }
}
