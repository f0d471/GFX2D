#include "platform.h"
/* No unused writable Flash partition is assigned in the existing hardware
 * layout. Never pretend RAM is persistent or overwrite assets/application. */
int platform_store_write(const char *name,const void *data,uint32_t bytes)
{ (void)name;(void)data;(void)bytes;return PLATFORM_NOT_SUPPORTED; }
int platform_store_read(const char *name,void *data,uint32_t capacity,uint32_t *bytes)
{ (void)name;(void)data;(void)capacity;(void)bytes;return PLATFORM_NOT_SUPPORTED; }
