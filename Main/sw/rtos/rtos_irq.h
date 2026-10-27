/******************************************************************************
* @file rtos_irq.h: FreeRTOS 集成层的外部中断桥与帧节拍
*
* @brief  sw/rtos 是叠在 bsp 之上的 FreeRTOS 集成层（SW10）。本文件是它对应用
*         暴露的全部接口：
*           - rtos_irq_init      建 PLIC 与帧信号量（内核启动前调）
*           - rtos_trap_install  mtvec 指到 FreeRTOS 端口的 trap 入口
*                                （紧挨 vTaskStartScheduler 之前调，同厂商 demo）
*           - rtos_wait_frame    睡到下一个场起始（60Hz），任务级；超时返回 -1
*           - freertos_risc_v_application_interrupt_handler  内核端口的非 timer
*             中断派发点（.weak 覆盖，见 rtos_irq.c 文件头）
*
*         派发本体在 bsp/ti60/board_irq.c 的 board_irq_dispatch_external()：
*         裸机 trap() 与本桥共用同一段 claim 循环，A=场起始/B=引擎完成的
*         清源与应答纪律只维护一份。每个新到的场起始事件给一次帧信号量。
******************************************************************************/
#ifndef RTOS_IRQ_H
#define RTOS_IRQ_H

#include <stdint.h>

/* 建 PLIC（含场起始源使能）与帧信号量；返回 board_irq_init 的结果。 */
int  rtos_irq_init(void);

/* mtvec -> freertos_risc_v_trap_handler。必须在 vTaskStartScheduler 之前、
   且尽量紧挨着它调：装好之后到调度器起来之前若来外部中断，由 FreeRTOS
   端口入口 -> external_interrupt_handler 正常派发。 */
void rtos_trap_install(void);

/* 睡到下一个场起始事件；ms 毫秒内没等到返回 -1（50ms=三场余量，
   与 bsp 里轮询等待的上界同一口径）。 */
int  rtos_wait_frame(uint32_t ms);

#endif /* RTOS_IRQ_H */
