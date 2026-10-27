/******************************************************************************
* @file board_irq.c: display 场起始与引擎完成的项目中断入口
*
* @brief SDK 的 trap.S 只保存现场并调用 trap()；中断源、PLIC 和 CSR 的
*         初始化与确认均留在 BSP。ISR 不输出串口，也不执行游戏逻辑。
******************************************************************************/

#include <stdint.h>

#include "board.h"
#include "gfx_regs.h"

#include "bsp.h"
#include "plic.h"
#include "riscv.h"

#include "display_regs.h"

static volatile uint32_t s_frames, s_engines, s_faults;
static volatile int s_enabled;

static uint32_t mmio_read(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static void mmio_write(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

void trap_entry(void);

void trap(void)
{
    const uint32_t cause = (uint32_t)csr_read(mcause);

    if (cause != (0x80000000u | CAUSE_MACHINE_EXTERNAL)) {
        s_faults++;
        for (;;) { }
    }
    board_irq_dispatch_external();
}

/* PLIC claim 循环本体。SW10 起有两个调用方：trap()（裸机，mtvec 直指）与
   sw/rtos 的 FreeRTOS 外部中断桥（portasmHANDLE_INTERRUPT -> 这里）。
   单独成函数不是为了复用方便，是为了 FreeRTOS 路径与裸机路径走同一段派发，
   A=场起始/B=引擎完成的清源与应答纪律只维护一份。 */
void board_irq_dispatch_external(void)
{
    uint32_t claim;

    while ((claim = plic_claim(BSP_PLIC, BSP_PLIC_CPU_0)) != 0u) {
        if (claim == SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT) {
            const uint32_t stat = mmio_read(DISP_IRQ_STAT) & 1u;
            if (stat != 0u) {
                mmio_write(DISP_IRQ_STAT, stat);
                s_frames++;
            }
        } else if (claim == SYSTEM_PLIC_USER_INTERRUPT_B_INTERRUPT) {
            const uint32_t stat = mmio_read(IO_APB_SLAVE_0_INPUT + GFX_IRQ_STAT)
                                & (GFX_IRQ_LIST_DONE | GFX_IRQ_ERROR);
            if (stat != 0u) {
#ifndef INJECT_SW9_SKIP_CLEAR_B
                mmio_write(IO_APB_SLAVE_0_INPUT + GFX_IRQ_STAT, stat);
#endif
                s_engines++;
            }
        } else {
            s_faults++;
        }
        /* 电平源先写 1 清，再向 PLIC 完成应答。 */
        plic_release(BSP_PLIC, BSP_PLIC_CPU_0, claim);
    }
}

int board_irq_init(void)
{
    if (mmio_read(DISP_ID) != DISP_ID_VALUE)
        return -1;

    s_enabled = 0;
    mmio_write(DISP_IRQ_EN, 0u);
    mmio_write(DISP_IRQ_STAT, 1u);
    mmio_write(IO_APB_SLAVE_0_INPUT + GFX_IRQ_EN, 0u);
    mmio_write(IO_APB_SLAVE_0_INPUT + GFX_IRQ_STAT,
               GFX_IRQ_LIST_DONE | GFX_IRQ_ERROR);
    s_frames = 0u;
    s_engines = 0u;
    s_faults = 0u;
    plic_set_threshold(BSP_PLIC, BSP_PLIC_CPU_0, 0u);
    plic_set_priority(BSP_PLIC, SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT, 1u);
    plic_set_priority(BSP_PLIC, SYSTEM_PLIC_USER_INTERRUPT_B_INTERRUPT, 1u);
    plic_set_enable(BSP_PLIC, BSP_PLIC_CPU_0, SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT, 1u);
    plic_set_enable(BSP_PLIC, BSP_PLIC_CPU_0, SYSTEM_PLIC_USER_INTERRUPT_B_INTERRUPT, 1u);
    csr_write(mtvec, trap_entry);
    csr_set(mie, MIE_MEIE);
    csr_set(mstatus, MSTATUS_MIE);
    s_enabled = 1;
#ifndef INJECT_SW9_NO_IRQ_A
    mmio_write(DISP_IRQ_EN, 1u);
#endif
    return 0;
}

int board_irq_enabled(void) { return s_enabled; }

void board_engine_irq_enable(void)
{
    mmio_write(IO_APB_SLAVE_0_INPUT + GFX_IRQ_STAT,
               GFX_IRQ_LIST_DONE | GFX_IRQ_ERROR);
#ifndef INJECT_SW9_NO_IRQ_B
    mmio_write(IO_APB_SLAVE_0_INPUT + GFX_IRQ_EN,
               GFX_IRQ_LIST_DONE | GFX_IRQ_ERROR);
#endif
}

uint32_t board_irq_frame_events(void) { return s_frames; }
uint32_t board_irq_engine_events(void) { return s_engines; }
uint32_t board_irq_faults(void) { return s_faults; }
