#ifndef CORE_HEALTH_H
#define CORE_HEALTH_H
#include <stdint.h>
#define HEALTH_CLIENTS 8u
typedef struct { uint32_t last,timeout,progress; int registered; } health_client;
typedef struct { health_client clients[HEALTH_CLIENTS]; uint32_t faults,recoveries,failed; int unhealthy; } health_monitor;
void health_init(health_monitor *h);
int health_register(health_monitor *h,unsigned id,uint32_t now,uint32_t timeout);
int health_progress(health_monitor *h,unsigned id,uint32_t now,uint32_t progress);
/* timeout must be < 2^31 ms. Caller checks at least once per timeout interval. */
int health_check(health_monitor *h,uint32_t now);
#endif
