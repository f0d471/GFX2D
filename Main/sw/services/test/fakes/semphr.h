#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned maximum,unsigned initial);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore,BaseType_t *wake);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,unsigned timeout);
