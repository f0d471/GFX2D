#define main test_entry
#include "../../../sim/engine/sh/test_sh.c"
#undef main
static unsigned draw(int split_depth)
{
    sh_world_init(42);memset(s_enemy,0,sizeof s_enemy);s_px=0;s_pz=-37*SH_ONE/10;s_yaw=32768u;
    fx16_mat4 p;CHECK(!fx16_perspective(&p,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z));
    rast_viewport vp={0,0,640,480};rast_frame frame={640,480};
    uint32_t n=sh_world_emit(&p,&vp,records,SH_PC_RECORD_CAP),split=sh_world_weapon_start();
    CHECK(n!=UINT32_MAX);memset(image,0,sizeof image);
    for(unsigned strip=0;strip<30;++strip) {
        rast_target t={0};t.color=image+strip*16u*640;t.depth=depth_strip;t.stride=640;t.strip=strip;
        memset(depth_strip,255,sizeof depth_strip);
        if(split_depth) {
            CHECK(!rast_ref_run_frame(records,split,&t,&frame));memset(depth_strip,255,sizeof depth_strip);
            CHECK(!rast_ref_run_frame(records+split,n-split,&t,&frame));
        }else CHECK(!rast_ref_run_frame(records,n,&t,&frame));
    }
    unsigned dark=0;for(unsigned y=300;y<430;++y)for(unsigned x=380;x<600;++x)
        if((image[y*640+x]&0xffffffu)<0x505050u)++dark;
    return dark;
}
int main(void){unsigned a=draw(0),b=draw(1);printf("SH WALL MODEL shared-depth=%u independent-depth=%u dark gun pixels\n",a,b);return 0;}
