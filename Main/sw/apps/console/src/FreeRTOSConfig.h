/******************************************************************************
* @file FreeRTOSConfig.h: console 的内核配置（SW10）
*
* @brief  自包含（不引 sdk 头）：板级事实写死并注明出处——
*           CLINT 基址 0xf8b00000（soc.h SYSTEM_CLINT_CTRL），
*           core_clk 100MHz（board.h BOARD_HZ）。
*         硬件走 RISCV_MTIME_CLINT_no_extensions 芯片头。
*
*         🔴 tick 源（2026-10-26 板上实测）：本 SoC 构型的 CLINT 只实现
*         mtime 计数器，**mtimecmp 写入丢失（CPU 读回恒 0）、machine timer
*         中断从不触发**——内核默认的 mtimecmp tick 源不可用。tick 改由
*         场起始中断 A（60Hz，sw/rtos 的桥内模拟端口 tick 路径）驱动，
*         因此 configTICK_RATE_HZ=60，pdMS_TO_TICKS 的粒度是一场 16.7ms；
*         vTaskDelay 的使用方都按整场取整（inputd 每场采样一次，与裸机
*         每帧读一次按键的节奏一致）。产品线若要毫秒级 tick，须在 SoC IP
*         层修 CLINT/USER_TIMER0 并重综合（列为后续工作点，不动 B7）。
*
*         任务规划（appfw）：runner=64K 栈（对齐裸机 64K 主栈的量级），
*         inputd=2K，monitor=4K，idle=缺省；内核堆 128K（DDR 充足）。
******************************************************************************/
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configTICK_RATE_HZ                      ((TickType_t)60)
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

/* 板级事实（见文件头）：mtime 可读（board_cycles 的基准），mtimecmp 在本
   构型无实现；内核的 vPortSetupTimerInterrupt 写 cmp 会落到空地址（无害），
   tick 实际由 sw/rtos 的场中断桥驱动。地址保留给内核编译（不能为 0，
   为 0 时端口会走另一条编译路径）。 */
#define configMTIME_BASE_ADDRESS                (0xf8b00000UL + 0xBFF8UL)
#define configMTIMECMP_BASE_ADDRESS             (0xf8b00000UL + 0x4000UL)
#define configCPU_CLOCK_HZ                      (100000000UL)

void vAssertCalled(const char *file, int line);
#define configASSERT(x)   do { if (!(x)) vAssertCalled(__FILE__, __LINE__); } while (0)

#endif /* FREERTOS_CONFIG_H */
