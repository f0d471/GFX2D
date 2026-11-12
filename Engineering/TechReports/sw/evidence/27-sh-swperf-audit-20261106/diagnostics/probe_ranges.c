#include "sh_scene.c"
#include <stdio.h>

static uint32_t arena[SH_CACHE_BYTES/4u];
static rast_record rec[SH_PC_RECORD_CAP], saved[SH_PC_RECORD_CAP];
static uint8_t bins[32768];
static unsigned warm(const uint8_t *ranges,unsigned count)
{
    unsigned result=0;
    for(unsigned i=0;i<count;++i)result+=ranges[2u*i]!=0xFFu;
    return result;
}
int main(void)
{
    sh_world_cache_bind(arena,sizeof arena);
    sh_world_init(7);
    fx16_mat4 projection;
    if(fx16_perspective(&projection,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z))return 2;
    rast_viewport vp={0,0,640,480};rast_frame frame={640,480};
    unsigned count=sh_world_emit(&projection,&vp,rec,SH_PC_RECORD_CAP);
    if(!count || count==UINT32_MAX || !s_room_cache.valid)return 2;
    memcpy(saved,rec,count*sizeof *rec);
    int bytes=rast_bin_frame_cache(rec,count,sh_world_ranges(),bins,sizeof bins,&frame);
    unsigned after_bin=warm(sh_world_ranges(),count);
    unsigned room_cached_warm=warm(s_room_cache.ranges,s_room_cache.count);
    unsigned room_count=s_room_cache.count,version=s_room_cache.version;
    unsigned next=sh_world_emit(&projection,&vp,rec,SH_PC_RECORD_CAP);
    unsigned next_warm=warm(sh_world_ranges(),next);
    int records_equal=next==count && !memcmp(saved,rec,count*sizeof *rec);
    printf("SW23_RANGE_PROBE records=%u bin_bytes=%d warm_after_bin=%u room_records=%u room_cached_warm=%u next_emit_warm=%u room_hit=%u records_equal=%u\n",
        count,bytes,after_bin,room_count,room_cached_warm,next_warm,
        version==s_room_cache.version,records_equal);
    if(bytes<120 || after_bin!=count || !records_equal || version!=s_room_cache.version)return 2;
    puts(next_warm==0?"REPRODUCED: scene cache discards every validated strip range":"Range reuse observed");
    return 0;
}
