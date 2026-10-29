/* PC-only scheduler probe, not a simulated FreeRTOS kernel. */
#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t StackType_t;
typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
void test_yield_from_isr(int wake);
#define portYIELD_FROM_ISR(wake) test_yield_from_isr(wake)
#define taskDISABLE_INTERRUPTS() test_enter()
#define configTICK_RATE_HZ 1000u
size_t xPortGetFreeHeapSize(void);
void test_enter(void);
void test_exit(void);
#define taskENTER_CRITICAL() test_enter()
#define taskEXIT_CRITICAL() test_exit()
#endif
