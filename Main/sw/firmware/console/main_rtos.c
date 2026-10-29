/* Ti60 scheduler port. Product logic lives in appfw.c and is also used on PC. */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "board.h"
#include "core/crc32.h"
#include "core/format.h"
#include "rtos_irq.h"
#include "console_content.h"
#include "app_port.h"

static void cooperate(void)
{
    if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING)(void)rtos_wait_frame(1);
}
static void engine_cooperate(void)
{
    if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING)(void)rtos_wait_engine(1);
}
static void input_task(void *arg)
{
    (void)arg;
    for(;;) {
        app_port_input_step();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
static void monitor_task(void *arg)
{
    (void)arg;
    for(;;) { vTaskDelay(pdMS_TO_TICKS(1000));app_port_monitor_step(); }
}
static void runner_task(void *arg)
{
    (void)arg;
    board_set_wait_hook(cooperate);
    board_set_engine_wait_hook(engine_cooperate);
    if(appfw_init(console_content_config(),APPFW_MENU)!=0) {
        board_puts("console fatal boot\r\n");
        for(;;)vTaskDelay(pdMS_TO_TICKS(1000));
    }
    app_port_runner_progress();
    for(;;) {
        (void)appfw_step(app_port_keys());
        /* Even CPU reference rendering yields between frames. Short hardware
         * waits remain bounded; display waits also use the cooperation hook. */
        app_port_runner_progress();app_port_idle();
    }
}
int main(void)
{
    board_init();board_product_keys(1);crc32_init();app_port_system_init();
    if(rtos_irq_init()!=0) { board_puts("console fatal irq\r\n");for(;;){} }
    if(xTaskCreate(input_task,"inputd",512,NULL,3,NULL)!=pdPASS ||
       xTaskCreate(runner_task,"runner",16384,NULL,2,NULL)!=pdPASS ||
       xTaskCreate(monitor_task,"monitor",1024,NULL,1,NULL)!=pdPASS) {
        board_puts("console fatal tasks\r\n");for(;;){}
    }
    rtos_trap_install();vTaskStartScheduler();
    for(;;){}
}
void vApplicationIdleHook(void) { app_port_idle_hook(); }
void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();board_puts("console fatal malloc\r\n");for(;;){}
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{
    (void)task;(void)name;
    taskDISABLE_INTERRUPTS();board_puts("console fatal stack\r\n");for(;;){}
}
void vAssertCalled(const char *file,int line)
{
    char text[160];core_out out;
    taskDISABLE_INTERRUPTS();core_out_init(&out,text,sizeof text);
    core_out_str(&out,"console fatal assert ");core_out_str(&out,file);
    core_out_ch(&out,' ');core_out_u32(&out,(uint32_t)line);
    core_out_str(&out,"\r\n");board_puts(text);for(;;){}
}
