/* R3 review experiment: current R1 code on the existing B0, no 3D RTL. */
#include "board.h"
#include "gfx/rast_setup.h"

static _Alignas(32) rast_record frame[1100];
static _Alignas(16) uint8_t sub[120 + 2 * 30 * 1100];
static rast_record temp[RAST_MAX_FAN];
static volatile uint32_t sink;

static void number(uint32_t x)
{
    char b[11]; unsigned n = 0;
    do { b[n++] = (char)('0' + x % 10); x /= 10; } while (x);
    while (n) { char s[2] = {b[--n], 0}; board_puts(s); }
}
static void row(const char *name, uint32_t cycles, uint32_t count)
{
    board_puts(name); board_puts(" cycles="); number(cycles);
    board_puts(" count="); number(count); board_puts(" per_call=");
    number(cycles / count); board_puts("\r\n");
}
int main(void)
{
    fx16_mat4 projection;
    rast_view_vertex view[3] = {{-65536,65536,-2*65536,0,0,0xff804020},
        {65536,65536,-2*65536,496,0,0xff408020},
        {-65536,-65536,-2*65536,0,496,0xff204080}};
    rast_vertex screen[3] = {{2560,1920,10000,0,0,0xff804020},
        {7680,1920,20000,496,0,0xff408020},
        {2560,5760,30000,0,496,0xff204080}};
    board_init();
    /* Keep the final result after the loader's collection boundary. */
    uint32_t start=board_cycles();
    while ((uint32_t)(board_cycles()-start)<BOARD_HZ*2u) {}
    board_puts("R3_SETUP_PROBE v2 O2 current R1 on B0\r\n");
    unsigned fail = fx16_perspective(&projection,16384,65536,65536,9*65536) != 0;
    const unsigned sizes[] = {16,32,64,1100};
    for (unsigned si=0; si<sizeof sizes/sizeof sizes[0]; ++si) {
    unsigned count=sizes[si];
    for (unsigned batch=0; batch<3; ++batch) {
        uint32_t t=board_cycles();
        for (unsigned i=0;i<count;++i) {
            screen[0].z=(uint16_t)(10000+i%101);
            int n=rast_setup_polygon(screen,3,RAST_VCOLOR_EN|RAST_TEX_EN,
                                     temp,RAST_MAX_FAN,0);
            if(n!=1) ++fail;
            frame[i]=temp[0]; sink^=rast_get32(temp,44);
        }
        row("polygon",board_cycles()-t,count);
        t=board_cycles();
        for (unsigned i=0;i<count;++i) {
            view[0].z=-2*65536-(int32_t)(i%101);
            int n=rast_setup_view(view,&projection,65536,9*65536,
                RAST_VCOLOR_EN|RAST_TEX_EN,temp,RAST_MAX_FAN,0);
            if(n!=1) ++fail;
            sink^=rast_get32(temp,44);
        }
        row("view",board_cycles()-t,count);
        t=board_cycles();
        int bytes=rast_bin(frame,count,sub,sizeof sub);
        row("bin",board_cycles()-t,count);
        if(bytes<0) ++fail;
        board_puts("sub_bytes="); number((uint32_t)bytes); board_puts("\r\n");
    }
    }
    board_puts("sink=");number(sink);board_puts("\r\nfail ");number(fail);
    board_puts("\r\ndone\r\n");
    return fail ? 1:0;
}
