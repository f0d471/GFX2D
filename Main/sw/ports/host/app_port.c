#include <string.h>
#include "board.h"
#include "app_port.h"
static uint32_t s_keys,s_idle;
uint32_t app_port_ms(void) { return (uint32_t)board_time_ms(); }
uint32_t app_port_keys(void) { s_keys=board_keys();return s_keys; }
uint32_t app_port_held(void) { return s_keys; }
int app_port_hardware(void) { return 0; }
void app_port_snapshot(app_port_stats *p)
{ memset(p,0,sizeof *p);p->tick_ms=app_port_ms();p->tasks=1;p->idle=s_idle; }
void app_port_idle(void) { ++s_idle; }
uint32_t app_port_swap_count(void) { return board_swap_count(); }
uint32_t app_port_frame_events(void) { return board_irq_frame_events(); }
uint32_t app_port_faults(void) { return board_irq_faults(); }
uint32_t app_port_tx_room(void) { return board_tx_room(); }
void app_port_puts(const char *s) { board_puts(s); }
void app_port_wait_frame(void) { board_wait_frame(); }
void app_port_yield(void) { board_wait_yield(); }
void app_port_override_keys(uint32_t keys) { s_keys=keys; }
void app_port_system_init(void) { s_idle=s_keys=0; }
void app_port_runner_progress(void) { }
