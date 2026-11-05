#define main scene_test_main
#include "../../engine/sh/test_sh.c"
#undef main
int main(void)
{
    fx16_mat4 p;CHECK(!fx16_perspective(&p,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z));
    rast_viewport vp={0,0,640,480};
    for(int x=-1;x<=1;++x)for(int z=-1;z<=1;++z)for(unsigned yaw=0;yaw<32;++yaw) {
        sh_world_init(42);s_px=x*48*SH_ONE/10;s_pz=z*38*SH_ONE/10;s_yaw=(uint16_t)(yaw*2048);
        memset(s_enemy,0,sizeof s_enemy);
        uint32_t n=sh_world_emit(&p,&vp,records,SH_PC_RECORD_CAP),split=sh_world_weapon_start();
        CHECK(n!=UINT32_MAX);
        if(!split && n) {printf("SH EMPTY WORLD REPRO x=%d z=%d yaw=%u records=%u weapon=%u\n",s_px,s_pz,s_yaw,n,n-split);return 0;}
    }
    return 1;
}
