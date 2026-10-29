#include <string.h>
#include "core/record.h"
#include "core/crc32.h"
static void put(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint32_t crc(const void *p,uint32_t n) { crc32_ctx c;crc32_begin(&c);crc32_update(&c,p,n);return crc32_end(&c); }
int record_encode(void *dst,uint32_t cap,const void *payload,uint32_t n,uint32_t seq)
{
    uint8_t *p=dst;
    if(!p || (!payload&&n) || cap<RECORD_HEADER_BYTES || n>cap-RECORD_HEADER_BYTES)return -1;
    if(n)memmove(p+RECORD_HEADER_BYTES,payload,n);
    put(p,0x3152564eu);put(p+4,1);put(p+8,seq);put(p+12,n);
    put(p+16,crc(p+RECORD_HEADER_BYTES,n));put(p+20,crc(p,20));return 0;
}
int record_validate(const void *src,uint32_t bytes,uint32_t *seq,uint32_t *length)
{
    const uint8_t *p=src;uint32_t n;
    if(!p || bytes<RECORD_HEADER_BYTES || get(p)!=0x3152564eu || get(p+4)!=1 || get(p+20)!=crc(p,20))return -1;
    n=get(p+12);
    if(n!=bytes-RECORD_HEADER_BYTES || get(p+16)!=crc(p+RECORD_HEADER_BYTES,n))return -1;
    if(seq)*seq=get(p+8);
    if(length)*length=n;
    return 0;
}
int record_select(const void *a,uint32_t an,const void *b,uint32_t bn)
{
    uint32_t x=0,y=0;int av=record_validate(a,an,&x,NULL)==0,bv=record_validate(b,bn,&y,NULL)==0;
    if(!av)return bv?1:-1;
    if(!bv)return 0;
    return (uint32_t)(y-x)!=0 && (uint32_t)(y-x)<0x80000000u?1:0;
}
