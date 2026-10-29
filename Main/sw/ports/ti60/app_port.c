#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "board.h"
#include "app_port.h"
#include "core/health.h"
static volatile uint32_t s_pending,s_held,s_idle,s_monitor,s_input;
static uint32_t s_stack=UINT32_MAX;
static uint32_t s_stack_sample;
static int s_runner_ready;
static health_monitor s_health;
uint32_t app_port_ms(void) { return (uint32_t)((uint64_t)xTaskGetTickCount()*1000u/configTICK_RATE_HZ); }
int app_port_hardware(void) { return 1; }
uint32_t app_port_keys(void)
{ uint32_t keys;taskENTER_CRITICAL();keys=s_pending;s_pending=s_held;taskEXIT_CRITICAL();return keys; }
uint32_t app_port_held(void) { return s_held; }
void app_port_snapshot(app_port_stats *p)
{ *p=(app_port_stats){app_port_ms(),(uint32_t)xPortGetFreeHeapSize(),(uint32_t)uxTaskGetNumberOfTasks(),s_idle,s_monitor,s_health.faults,s_stack}; }
void app_port_system_init(void)
{
    health_init(&s_health);
    s_runner_ready=0;
    (void)health_register(&s_health,0,0,3000);
    (void)health_register(&s_health,1,0,30000); /* Flash resource boot has a separate budget. */
}
void app_port_input_step(void)
{
    /* UART drain/CRC has a 64-byte budget and runs with interrupts enabled. */
    uint32_t keys=board_keys();
    taskENTER_CRITICAL();s_pending|=keys;s_held=keys;++s_input;taskEXIT_CRITICAL();
}
void app_port_runner_progress(void)
{
    if(!s_runner_ready) {
        (void)health_register(&s_health,1,app_port_ms(),3000);s_runner_ready=1;
    }
    if(s_stack==UINT32_MAX || (uint32_t)(app_port_ms()-s_stack_sample)>=1000u) {
        uint32_t water=(uint32_t)uxTaskGetStackHighWaterMark(NULL)*sizeof(StackType_t);
        s_stack_sample=app_port_ms();
        if(water<s_stack)s_stack=water;
    }
}
void app_port_monitor_step(void)
{
    uint32_t now=app_port_ms(),old=s_health.faults;++s_monitor;
    (void)health_progress(&s_health,0,now,s_input);
    /* Completed output is meaningful progress; a spinning fault loop is not. */
    (void)health_progress(&s_health,1,now,board_swap_count());
    (void)health_check(&s_health,now);
    if(s_health.faults!=old)board_puts("system health stalled\r\n");
}
void app_port_idle(void) { vTaskDelay(1); }
void app_port_idle_hook(void) { ++s_idle; }
uint32_t app_port_swap_count(void) { return board_swap_count(); }
uint32_t app_port_frame_events(void) { return board_irq_frame_events(); }
uint32_t app_port_faults(void) { return board_irq_faults(); }
uint32_t app_port_tx_room(void) { return board_tx_room(); }
void app_port_puts(const char *s) { board_puts(s); }
void app_port_wait_frame(void) { board_wait_frame(); }
void app_port_yield(void) { board_wait_yield(); }
