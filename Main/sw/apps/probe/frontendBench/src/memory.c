/* F1 受控访存：相同记录、地址集合、写次数，改变访问次序。 */
#include "board.h"
#include "gfx/gfx.h"
#include "core/format.h"
#include <stdlib.h>
#include <string.h>
static uint32_t times[1000],sink;
static int cmp(const void *a,const void *b){uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;return x>y?1:x<y?-1:0;}
static void field(uint32_t v){char b[16];core_out o;core_out_init(&o,b,sizeof b);core_out_u32(&o,v);board_puts(",");board_puts(b);}
/* 单独函数保留实际八字存储；调用者不启用 LTO。 */
void frontend_store_stream(uint8_t *table,const gfx_cmd *in,uint32_t order)
{
    uint32_t a,b;
    for(a=0;a<(order==2u?128u:30u);a++)for(b=0;b<(order==2u?30u:128u);b++) {
        uint32_t s=order==2u?b:order==1u?(a*7u)%30u:a,j=order==2u?a:b;
        ((gfx_cmd *)(void *)(table+(s+1u)*32768u))[j]=in[s*128u+j];
    }
}
static uint32_t read_stream(const uint8_t *table,uint32_t order,int rewrite)
{
    uint32_t a,b,total=0;
    for(a=0;a<(order==2u?128u:30u);a++)for(b=0;b<(order==2u?30u:128u);b++) {
        uint32_t s=order==2u?b:order==1u?(a*7u)%30u:a,j=order==2u?a:b,k;
        uint32_t *p=(uint32_t *)(void *)(table+(s+1u)*32768u+j*32u);
        for(k=0;k<8u;k++){uint32_t v=p[k];total+=v;if(rewrite)p[k]=v^0x01010101u;}
    }
    return total;
}
int frontend_memory_bench(void)
{
    board_mem mem=board_region(REGION_CMDLIST_A),shared=board_region(REGION_SHARED);
    gfx_cmd *in=(gfx_cmd *)(void *)((uint8_t *)shared.ptr+0x400000u);
    uint32_t i,j,s,order,kind,cold,r,expected=0,changed=0,fail=0;
    for(i=0;i<3840u;i++)for(j=0;j<8u;j++){in[i].w[j]=(i*37u)^(j*0x1234567u);expected+=in[i].w[j];changed+=in[i].w[j]^0x01010101u;}
    for(cold=0;cold<2u;cold++)for(kind=0;kind<3u;kind++)for(order=0;order<3u;order++)for(r=0;r<3u;r++) {
        uint64_t total=0;
        frontend_store_stream(mem.ptr,in,0);read_stream(mem.ptr,0,0);
        for(i=0;i<1000u;i++) {
            uint32_t start;
            if(cold)board_dcache_invalidate_all();
            start=board_cycles();
            if(!kind)frontend_store_stream(mem.ptr,in,order);
            else sink=read_stream(mem.ptr,order,kind==2u);
            times[i]=board_cycles()-start;total+=times[i];
        }
        for(s=0;s<30u;s++)if(memcmp((uint8_t *)mem.ptr+(s+1u)*32768u,in+s*128u,4096u))fail++;
        if(kind&&sink!=(kind==2u?changed:expected))fail++;
        qsort(times,1000u,4u,cmp);
        board_puts("fmem");field(cold);field(kind);field(order);field(r);field(1000u);
        field((uint32_t)(total>>32));field((uint32_t)total);field(times[500]);field(times[990]);field(times[999]);field(fail);board_puts("\n");
    }
    return fail?1:0;
}
int frontend_sets_bench(void)
{
    uint32_t *base=(uint32_t *)board_region(REGION_CMDLIST_A).ptr;
    uint32_t same,cold,kind,r,i,j,fail=0;
    for(same=0;same<2u;same++)for(cold=0;cold<2u;cold++)for(kind=0;kind<3u;kind++)for(r=0;r<3u;r++) {
        uint32_t step=same?1024u:16u,expected=0,changed=0;
        uint64_t total=0;
        for(j=0;j<64u;j++){uint32_t v=0x12340000u+j;base[j*step]=v;expected+=v;changed+=v^0x01010101u;}
        sink=0;for(j=0;j<64u;j++)sink+=base[j*step];
        for(i=0;i<1000u;i++) {
            uint32_t start,sum=0;
            if(cold)board_dcache_invalidate_all();
            start=board_cycles();
            for(j=0;j<64u;j++) {
                if(kind==1u)base[j*step]=0x12340000u+j;
                else {uint32_t v=base[j*step];sum+=v;if(kind==2u)base[j*step]=v^0x01010101u;}
            }
            times[i]=board_cycles()-start;total+=times[i];sink=sum;
        }
        for(j=0;j<64u;j++)if(base[j*step]!=0x12340000u+j)fail++;
        if(kind!=1u&&sink!=(kind==2u?changed:expected))fail++;
        qsort(times,1000u,4u,cmp);
        board_puts("fsets");field(same);field(cold);field(kind);field(r);field(1000u);
        field((uint32_t)(total>>32));field((uint32_t)total);field(times[500]);field(times[990]);field(times[999]);field(fail);board_puts("\n");
    }
    return fail?1:0;
}
