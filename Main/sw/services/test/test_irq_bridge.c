#include <stdio.h>
#include "board.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "rtos_irq.h"
static unsigned counts[2],allocated,frames,engines,dispatch_frame,dispatch_engine,yields;
static int running;
uint32_t test_mcause=0x8000000bu;
void freertos_risc_v_trap_handler(void) {}
int board_irq_init(void) { return 0; }
uint32_t board_irq_frame_events(void) { return frames; }
uint32_t board_irq_engine_events(void) { return engines; }
void board_irq_dispatch_external(void) { frames+=dispatch_frame;engines+=dispatch_engine; }
int xTaskGetSchedulerState(void) { return running; }
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned maximum,unsigned initial)
{ (void)maximum;counts[allocated]=initial;return &counts[allocated++]; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *wake)
{ ++*(unsigned *)s;*wake=1;return 1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,unsigned timeout)
{ (void)timeout;if(!*(unsigned *)s)return 0;--*(unsigned *)s;return 1; }
void test_yield_from_isr(int wake) { yields+=(unsigned)wake; }
void test_enter(void) {}
void board_puts(const char *s) { (void)s; }
void freertos_risc_v_application_interrupt_handler(void);
#define CHECK(c) do { if(!(c)) { printf("FAIL irq line %d\n",__LINE__);return 1; } } while(0)
int main(void)
{
    CHECK(rtos_irq_init()==0 && allocated==2);
    dispatch_frame=dispatch_engine=1;
    freertos_risc_v_application_interrupt_handler();
    CHECK(!counts[0] && !counts[1]);
    running=taskSCHEDULER_RUNNING;dispatch_frame=0;
    freertos_risc_v_application_interrupt_handler();
    CHECK(rtos_wait_frame(1)==-1 && rtos_wait_engine(1)==0 && yields==1);
    dispatch_frame=1;dispatch_engine=0;
    freertos_risc_v_application_interrupt_handler();
    CHECK(rtos_wait_frame(1)==0 && rtos_wait_engine(1)==-1 && yields==2);
    puts("actual IRQ bridge scheduler-gate/independent-events PASS");return 0;
}
