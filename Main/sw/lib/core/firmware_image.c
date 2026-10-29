#include <string.h>
#include "core/firmware_image.h"
#include "core/crc32.h"
static void put(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint32_t crc(const void *p,uint32_t n) { crc32_ctx c;crc32_begin(&c);crc32_update(&c,p,n);return crc32_end(&c); }
int firmware_image_encode(void *out,uint32_t cap,const void *image,uint32_t n,uint32_t target,uint32_t version)
{
    uint8_t *p=out;
    if(!p || !image || !n || cap<FIRMWARE_HEADER_BYTES || n>cap-FIRMWARE_HEADER_BYTES)return -1;
    memmove(p+FIRMWARE_HEADER_BYTES,image,n);
    put(p,0x31495746u);put(p+4,FIRMWARE_HEADER_BYTES);put(p+8,n);put(p+12,target);
    put(p+16,version);put(p+20,crc(p+FIRMWARE_HEADER_BYTES,n));put(p+24,0);put(p+28,crc(p,28));return 0;
}
int firmware_image_validate(const void *data,uint32_t bytes,uint32_t target,uint32_t min,uint32_t *image_bytes)
{
    const uint8_t *p=data;uint32_t n;
    if(!p || bytes<FIRMWARE_HEADER_BYTES || get(p)!=0x31495746u || get(p+4)!=FIRMWARE_HEADER_BYTES || get(p+24)!=0 ||
       get(p+12)!=target || get(p+16)<min || get(p+28)!=crc(p,28))return -1;
    n=get(p+8);
    if(!n || n!=bytes-FIRMWARE_HEADER_BYTES || get(p+20)!=crc(p+FIRMWARE_HEADER_BYTES,n))return -1;
    if(image_bytes)*image_bytes=n;
    return 0;
}
