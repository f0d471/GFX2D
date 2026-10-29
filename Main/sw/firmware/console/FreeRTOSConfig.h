/******************************************************************************
* @file FreeRTOSConfig.h: console 的内核配置（SW10）
*
* @brief  自包含（不引 sdk 头）：板级事实写死并注明出处——
*           CLINT 基址 0xf8b00000（soc.h SYSTEM_CLINT_CTRL），
*           core_clk 100MHz（board.h BOARD_HZ）。
*         硬件走 RISCV_MTIME_CLINT_no_extensions 芯片头。
*
*         tick 来自 CLINT machine timer，1kHz，与显示中断独立。
*         2026-10-28 独立板测确认 cmp 读回为 0 仍能正常触发 1kHz 中断；
*         已删除使用错误地址 0x0f8bbff8 的自定义初始化覆盖。
*         寄存器地址由 board_clock.h 提供，BSP 对生成的 soc.h 做静态断言。
*
*         任务规划（appfw）：runner=64K 栈（对齐裸机 64K 主栈的量级），
*         inputd=2K，monitor=4K，idle=缺省；内核堆 128K（DDR 充足）。
******************************************************************************/
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    6
#define configMINIMAL_STACK_SIZE                ((uint16_t)192)
#define configTOTAL_HEAP_SIZE                   ((size_t)128 * 1024)
#define configMAX_TASK_NAME_LEN                 12
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_TIMERS                        0
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configRECORD_STACK_HIGH_ADDRESS         1

/* 钩子：失败要立刻可见（断言/分配失败停下，比带着坏状态跑完一轮干净） */
#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2

#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     1

#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetSchedulerState          1

/* 标准 RISC-V 端口的独立 timer 配置；不用显示场中断模拟内核时间。 */
#include "board_clock.h"
#define configMTIME_BASE_ADDRESS                BOARD_MTIME_ADDRESS
#define configMTIMECMP_BASE_ADDRESS             BOARD_MTIMECMP_ADDRESS
#define configCPU_CLOCK_HZ                      BOARD_CLOCK_HZ

void vAssertCalled(const char *file, int line);
#define configASSERT(x)   do { if (!(x)) vAssertCalled(__FILE__, __LINE__); } while (0)

#endif /* FREERTOS_CONFIG_H */
