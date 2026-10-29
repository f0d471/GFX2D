#include <string.h>
#include "platform.h"
#include "board.h"
enum { FREE,ACQUIRED,PENDING,FRONT };
typedef struct { uint32_t generation,ticket; int state,mapped; board_frame view; } slot;
static slot s_slots[BOARD_FB_COUNT];
static int s_initialized,s_fault,s_closed,s_pending=-1,s_front=-1;
static uint32_t s_keys;
static slot *lookup(platform_frame f)
{
    if(!s_initialized || s_closed || f.slot>=BOARD_FB_COUNT || !f.generation ||
       s_slots[f.slot].generation!=f.generation)return NULL;
    return &s_slots[f.slot];
}
int platform_init(void)
{
    if(s_initialized)return PLATFORM_BUSY;
    board_init();board_product_keys(0);
    board_input_window_policy(0); /* Escape belongs to the SDK application. */
    memset(s_slots,0,sizeof s_slots);s_fault=0;s_pending=s_front=-1;s_keys=0;
    s_initialized=1;return PLATFORM_OK;
}
int platform_frame_acquire(platform_frame *frame)
{
    unsigned i;
    if(!frame || !s_initialized)return PLATFORM_INVALID;
    if(s_closed)return PLATFORM_CLOSED;
    if(s_fault)return PLATFORM_IO;
    if(board_should_quit())return PLATFORM_CLOSED;
    for(i=0;i<BOARD_FB_COUNT;i++)if(s_slots[i].state==FREE) {
        slot *p=&s_slots[i];
        if(board_frame_acquire((int)i,0,&p->view)!=0)return PLATFORM_IO;
        if(++p->generation==0)++p->generation;
        p->state=ACQUIRED;p->mapped=0;
        *frame=(platform_frame){i,p->generation};return PLATFORM_OK;
    }
    return PLATFORM_BUSY;
}
int platform_frame_map(platform_frame frame,platform_pixels *pixels)
{
    slot *p=lookup(frame);
    if(!p || !pixels || p->state!=ACQUIRED || p->mapped)return PLATFORM_INVALID;
    board_dcache_invalidate_range(p->view.pixels,p->view.stride*p->view.height*4u);
    p->mapped=1;
    *pixels=(platform_pixels){p->view.pixels,p->view.width,p->view.height,p->view.stride};
    return PLATFORM_OK;
}
int platform_frame_unmap(platform_frame frame)
{
    slot *p=lookup(frame);
    if(!p || p->state!=ACQUIRED || !p->mapped)return PLATFORM_INVALID;
    board_cpu_write_complete();p->mapped=0;return PLATFORM_OK;
}
int platform_frame_release(platform_frame frame)
{
    slot *p=lookup(frame);
    if(!p || p->state!=ACQUIRED || p->mapped)return PLATFORM_INVALID;
    p->state=FREE;return PLATFORM_OK;
}
int platform_frame_present(platform_frame frame)
{
    slot *p=lookup(frame);
    if(!p || p->state!=ACQUIRED || p->mapped)return PLATFORM_INVALID;
    if(s_fault)return PLATFORM_IO;
    if(s_pending>=0)return PLATFORM_BUSY;
    if(board_should_quit())return PLATFORM_CLOSED;
    if(board_present_frame_request((int)frame.slot,&p->view,&p->ticket)!=0)return PLATFORM_IO;
    p->state=PENDING;s_pending=(int)frame.slot;return PLATFORM_OK;
}
int platform_frame_wait(platform_frame frame,uint32_t timeout_ms)
{
    slot *p=lookup(frame);uint64_t start=board_time_ms();
    if(!p || p->state!=PENDING || s_pending!=(int)frame.slot)return PLATFORM_INVALID;
    while(!board_present_finished(p->ticket)) {
        if(board_should_quit()) { s_fault=1;return PLATFORM_CLOSED; }
        if(board_time_ms()-start>=timeout_ms) { s_fault=1;return PLATFORM_TIMEOUT; }
        board_wait_yield();
    }
    if(board_should_quit()) { s_fault=1;return PLATFORM_CLOSED; }
    if(board_swap_count()==p->ticket) { s_fault=1;return PLATFORM_IO; }
    if(s_front>=0)s_slots[s_front].state=FREE;
    s_front=(int)frame.slot;p->state=FRONT;s_pending=-1;return PLATFORM_OK;
}
int platform_shutdown(uint32_t timeout_ms)
{
    unsigned i;
    if(!s_initialized)return PLATFORM_INVALID;
    if(s_closed)return PLATFORM_CLOSED;
    if(s_fault)return PLATFORM_IO; /* Unknown scan ownership is never recycled. */
    for(i=0;i<BOARD_FB_COUNT;i++)if(s_slots[i].state==ACQUIRED)return PLATFORM_BUSY;
    if(s_pending>=0) {
        platform_frame f={(uint32_t)s_pending,s_slots[s_pending].generation};
        int rc=platform_frame_wait(f,timeout_ms);if(rc)return rc;
    }
    /* BSP/SDL lifetime belongs to the firmware; no re-init after shutdown. */
    s_closed=1;return PLATFORM_OK;
}
uint32_t platform_present_count(void) { return board_swap_count(); }
platform_input platform_input_poll(void)
{
    uint32_t keys=board_keys();platform_input p={keys,keys&~s_keys,s_keys&~keys};
    s_keys=keys;return p;
}
uint64_t platform_time_ms(void) { return board_time_ms(); }
int platform_sleep_ms(uint32_t ms)
{
    uint64_t start=board_time_ms();
    while(board_time_ms()-start<ms) {
        if(board_should_quit())return PLATFORM_CLOSED;
        board_wait_yield();
    }
    return PLATFORM_OK;
}
int platform_should_quit(void) { return board_should_quit(); }
void platform_log(const char *s) { if(s)board_puts(s); }
int platform_resource_read(uint32_t o,void *p,uint32_t n)
{
    if((!p && n) || o>BOARD_ASSET_MAX_BYTES || n>BOARD_ASSET_MAX_BYTES-o)return PLATFORM_RANGE;
    return board_asset_read(o,p,n)==0?PLATFORM_OK:PLATFORM_IO;
}
