#include <stdio.h>
#include "board.h"
#include "app_port.h"
static int critical,violation;
static uint32_t tick,keys,swaps;
void test_enter(void) { ++critical; }
void test_exit(void) { --critical; }
uint32_t board_keys(void) { if(critical)violation=1;return keys; }
uint32_t xTaskGetTickCount(void) { return tick; }
size_t xPortGetFreeHeapSize(void) { return 50000; }
unsigned uxTaskGetNumberOfTasks(void) { return 4; }
unsigned uxTaskGetStackHighWaterMark(void *p) { (void)p;return 128; }
void vTaskDelay(uint32_t n) { tick+=n; }
uint32_t board_swap_count(void) { return swaps; }
uint32_t board_irq_frame_events(void) { return 0; }
uint32_t board_irq_faults(void) { return 0; }
uint32_t board_tx_room(void) { return 32; }
void board_puts(const char *s) { (void)s; }
void board_wait_frame(void) {}
void board_wait_yield(void) {}
#define CHECK(c) do { if(!(c)) { printf("FAIL scheduler line %d\n",__LINE__);return 1; } } while(0)
int main(void)
{
    app_port_stats p;app_port_system_init();
    keys=1;app_port_input_step();keys=0;app_port_input_step();
    CHECK(!critical && !violation && app_port_keys()==1 && app_port_keys()==0);
    swaps=1;app_port_runner_progress();tick=1000;app_port_monitor_step();
    app_port_snapshot(&p);CHECK(p.health_faults==0 && p.stack_free==512);
    tick=2000;swaps=2;app_port_input_step();app_port_runner_progress();app_port_monitor_step();
    tick=3000;app_port_input_step();app_port_monitor_step();
    tick=4000;app_port_input_step();app_port_monitor_step();
    tick=5000;app_port_input_step();app_port_monitor_step();
    app_port_snapshot(&p);CHECK(p.health_faults==1);
    tick=6000;swaps=3;app_port_input_step();app_port_runner_progress();app_port_monitor_step();
    app_port_snapshot(&p);CHECK(p.health_faults==1 && !violation);
    tick=9000;app_port_input_step();app_port_monitor_step();
    app_port_snapshot(&p);CHECK(p.health_faults==2);
    puts("actual scheduler port critical-scope/health/stack PASS");return 0;
}
