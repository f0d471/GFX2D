/* Independent SDK consumer two. Position and game rules belong to this app. */
#include "platform.h"
int main(void)
{
    int x=8,y=10,vx=3,vy=2;
    uint64_t start;
    if(platform_init())return 1;
    start=platform_time_ms();
    for(unsigned frame=0;frame<24u;frame++) {
        platform_input input=platform_input_poll();platform_frame f;platform_pixels p;
        if(input.pressed&PLATFORM_KEY_BACK)break;
        if(platform_frame_acquire(&f) || platform_frame_map(f,&p))return 2;
        for(unsigned row=0;row<p.height;row++)for(unsigned col=0;col<p.width;col++)p.pixels[row*p.pitch+col]=0xff081020u;
        x+=vx;y+=vy;
        if(x<0) { x=0;vx=-vx; }if(y<0) { y=0;vy=-vy; }
        if((unsigned)(x+16)>p.width) { x=(int)p.width-16;vx=-vx; }
        if((unsigned)(y+16)>p.height) { y=(int)p.height-16;vy=-vy; }
        for(int row=y;row<y+16;row++)for(int col=x;col<x+16;col++)p.pixels[(unsigned)row*p.pitch+(unsigned)col]=0xffffa020u;
        if(platform_frame_unmap(f) || platform_frame_present(f) || platform_frame_wait(f,100))return 3;
        if(platform_time_ms()<start)return 4;
    }
    if(platform_shutdown(100) || platform_present_count()!=24)return 5;
    platform_log("SDK bounce frames 24 presents 24 PASS\n");return 0;
}
