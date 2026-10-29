#include "FreeRTOS.h"
uint32_t xTaskGetTickCount(void);
unsigned uxTaskGetNumberOfTasks(void);
unsigned uxTaskGetStackHighWaterMark(void *task);
void vTaskDelay(uint32_t ticks);
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING 1
int xTaskGetSchedulerState(void);
