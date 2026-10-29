/* Public SDK. No board, OS, accelerator, or game-private headers are required. */
#ifndef PLATFORM_API_H
#define PLATFORM_API_H
#include <stddef.h>
#include <stdint.h>
#include "input.h"
enum { PLATFORM_OK=0, PLATFORM_BUSY=-1, PLATFORM_INVALID=-2,
       PLATFORM_TIMEOUT=-3, PLATFORM_CLOSED=-4, PLATFORM_IO=-5,
       PLATFORM_NOT_SUPPORTED=-6, PLATFORM_CORRUPT=-7, PLATFORM_RANGE=-8 };
typedef struct { uint32_t slot,generation; } platform_frame;
typedef struct { uint32_t *pixels; unsigned width,height,pitch; } platform_pixels;
/* One display owner per firmware. A successful present transfers ownership;
 * wait confirms scanning and only then makes the old front buffer reusable.
 * Generation checks reject stale handles. Never retain pixels after unmap. */
int platform_init(void);
int platform_shutdown(uint32_t timeout_ms);
int platform_frame_acquire(platform_frame *frame);
int platform_frame_map(platform_frame frame,platform_pixels *pixels);
int platform_frame_unmap(platform_frame frame);
int platform_frame_release(platform_frame frame);
int platform_frame_present(platform_frame frame);
int platform_frame_wait(platform_frame frame,uint32_t timeout_ms);
uint32_t platform_present_count(void);
platform_input platform_input_poll(void);
uint64_t platform_time_ms(void);
int platform_sleep_ms(uint32_t ms);
int platform_should_quit(void);
void platform_log(const char *text);
/* Raw read-only resource bytes; asset formats remain library/content concerns. */
int platform_resource_read(uint32_t offset,void *dst,uint32_t bytes);
/* Atomic named record. Host uses two persistent slots. An unsupported target
 * reports NOT_SUPPORTED, never writes an assumed Flash partition. */
int platform_store_write(const char *name,const void *data,uint32_t bytes);
int platform_store_read(const char *name,void *data,uint32_t capacity,uint32_t *bytes);
#endif
