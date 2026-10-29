#include <string.h>
#include "core/health.h"
void health_init(health_monitor *h) { memset(h,0,sizeof *h); }
int health_register(health_monitor *h,unsigned id,uint32_t now,uint32_t timeout)
{
    if(!h || id>=HEALTH_CLIENTS || !timeout || timeout>=0x80000000u)return -1;
    h->clients[id]=(health_client){now,timeout,0,1};return 0;
}
int health_progress(health_monitor *h,unsigned id,uint32_t now,uint32_t progress)
{
    health_client *c;
    if(!h || id>=HEALTH_CLIENTS || !h->clients[id].registered)return -1;
    c=&h->clients[id];
    if(c->progress!=progress) { c->progress=progress;c->last=now; }
    return 0;
}
int health_check(health_monitor *h,uint32_t now)
{
    unsigned i;uint32_t failed=0;
    for(i=0;i<HEALTH_CLIENTS;i++)if(h->clients[i].registered &&
        (uint32_t)(now-h->clients[i].last)>=h->clients[i].timeout)failed|=1u<<i;
    if(failed && !h->unhealthy)++h->faults;
    if(!failed && h->unhealthy)++h->recoveries;
    h->failed=failed;h->unhealthy=failed!=0;return failed?-1:0;
}
