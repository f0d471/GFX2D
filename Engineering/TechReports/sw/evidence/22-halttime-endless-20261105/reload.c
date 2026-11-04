#define main original_test_main
#include "../../../sim/engine/sh/test_sh.c"
#undef main
int main(void)
{
    empty();enemy(3*SH_ONE,3*SH_ONE);s_ammo=1;s_cool=0;
    tick(PLATFORM_KEY_ACTION);CHECK(!s_ammo && s_reload);
    int32_t x=s_px,z=s_pz;uint32_t score=s_score;
    for(unsigned i=0;i<27;++i)tick(0);
    CHECK(s_ammo==6 && !s_reload && s_px==x && s_pz==z && s_score==score);
    puts("SH AUTO RELOAD PASS last round refills after 27 ticks without restart or pickup");return 0;
}
