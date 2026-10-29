/* Scheduler port. The application manager never includes FreeRTOS or SDL. */
#ifndef CONSOLE_PLATFORM_H
#define CONSOLE_PLATFORM_H
#include <stdint.h>
typedef struct {
    uint32_t tick_ms, heap_free, tasks, idle, monitor, health_faults, stack_free;
} app_port_stats;
uint32_t app_port_ms(void);
uint32_t app_port_keys(void);
uint32_t app_port_held(void);
int app_port_hardware(void);
void app_port_snapshot(app_port_stats *stats);
void app_port_idle(void);
uint32_t app_port_swap_count(void);
uint32_t app_port_frame_events(void);
uint32_t app_port_faults(void);
uint32_t app_port_tx_room(void);
void app_port_puts(const char *text);
void app_port_wait_frame(void);
void app_port_yield(void);
void app_port_system_init(void);
void app_port_input_step(void);
void app_port_runner_progress(void);
void app_port_monitor_step(void);
void app_port_idle_hook(void);
void app_port_override_keys(uint32_t keys);
#endif
