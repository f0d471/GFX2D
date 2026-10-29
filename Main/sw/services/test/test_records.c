#include <stdio.h>
#include <string.h>
#include "core/health.h"
#include "core/record.h"
#include "core/firmware_image.h"
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d\n",__LINE__);return 1; } } while(0)
int main(void)
{
    uint8_t a[96],b[96],bad[96],payload[64];uint32_t n,seq;
    for(unsigned i=0;i<sizeof payload;i++)payload[i]=(uint8_t)(i*37u+3u);
    CHECK(record_encode(a,sizeof a,payload,64,UINT32_MAX)==0);
    CHECK(record_encode(b,sizeof b,payload,64,0)==0);
    CHECK(record_validate(a,88,&seq,&n)==0 && seq==UINT32_MAX && n==64);
    CHECK(record_select(a,88,b,88)==1);
    for(unsigned i=0;i<88;i++)for(unsigned bit=0;bit<8;bit++) {
        memcpy(bad,a,88);bad[i]^=(uint8_t)(1u<<bit);
        CHECK(record_validate(bad,88,NULL,NULL)!=0);
    }
    for(unsigned i=0;i<88;i++)CHECK(record_validate(a,i,NULL,NULL)!=0);
    CHECK(record_select(a,88,b,87)==0 && record_select(a,1,b,2)==-1);
    CHECK(firmware_image_encode(a,sizeof a,payload,64,0x60,7)==0);
    CHECK(firmware_image_validate(a,96,0x60,7,&n)==0 && n==64);
    CHECK(firmware_image_validate(a,96,0x61,7,NULL)!=0);
    CHECK(firmware_image_validate(a,96,0x60,8,NULL)!=0);
    for(unsigned i=0;i<96;i++)for(unsigned bit=0;bit<8;bit++) {
        memcpy(bad,a,96);bad[i]^=(uint8_t)(1u<<bit);
        CHECK(firmware_image_validate(bad,96,0x60,0,NULL)!=0);
    }
    for(unsigned i=0;i<96;i++)CHECK(firmware_image_validate(a,i,0x60,0,NULL)!=0);
    health_monitor h;health_init(&h);
    CHECK(health_register(&h,0,UINT32_MAX-5u,10)==0);
    CHECK(health_register(&h,8,0,10)!=0 && health_register(&h,1,0,0)!=0);
    CHECK(health_check(&h,3)==0);
    CHECK(health_progress(&h,0,3,1)==0);
    CHECK(health_progress(&h,0,9,1)==0); /* Same progress must not feed health. */
    CHECK(health_check(&h,13)!=0 && h.failed==1 && h.faults==1);
    CHECK(health_check(&h,14)!=0 && h.faults==1);
    CHECK(health_progress(&h,0,14,2)==0 && health_check(&h,14)==0 && h.recoveries==1);
    puts("records corruption/sequence/firmware/health-wrap PASS");return 0;
}
