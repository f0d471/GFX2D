/* SDK consumer one: no BSP, graphics ABI, OS, or existing game headers. */
#include "platform.h"
int main(void)
{
    unsigned frames=0;
    if(platform_init())return 1;
    while(frames<16u && !platform_should_quit()) {
        platform_frame f;platform_pixels p;
        if(platform_frame_acquire(&f) || platform_frame_map(f,&p))return 2;
        for(unsigned y=0;y<p.height;y++)for(unsigned x=0;x<p.width;x++)
            p.pixels[y*p.pitch+x]=0xff000000u|((x+frames)&255u)<<16|((y+frames)&255u)<<8;
        if(platform_frame_unmap(f) || platform_frame_present(f) || platform_frame_wait(f,100))return 3;
        (void)platform_input_poll();++frames;
    }
    if(platform_shutdown(100) || platform_present_count()!=16)return 4;
    platform_log("SDK pixels frames 16 presents 16 PASS\n");return 0;
}
