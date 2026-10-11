/* F5 真实游戏帧：独立标量金标准、v1-I 输入、PB 字节、TEX 与 DDR 初值。 */
#include "gfx/gfx.h"
#include "gfx/pb.h"
#include "core/assetpak.h"
#include "game/stg_game.h"
#include "game/stg_kind.h"
#include "stg_modes.h"
#include "stg_slots.h"
#include "assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BYTES 0x800000u
#define FB 0x1000u
#define SRC 0x240000u
#define TABLE 0x600000u
#define INSTANCE 0x580000u
#define PB 0x700000u
static unsigned char memory[BYTES] __attribute__((aligned(128))), initial[BYTES], golden[BYTES], tex[16384];
static gfx_cmd flat[32768];
static stg_game game;
static const unsigned entries[13] = {ASSET_BUL16_ENTRY, ASSET_BUL8_ENTRY, ASSET_RICE20_ENTRY,
    ASSET_FONT8X16_ENTRY, ASSET_PSHOT_ENTRY, ASSET_SPARK_ENTRY, ASSET_SHIPS_ENTRY,
    ASSET_BOSS_ENTRY, ASSET_STARS_ENTRY, ASSET_TITLE_ENTRY, ASSET_DIGITS_ENTRY,
    ASSET_ICONS_ENTRY, ASSET_FONT_CYAN_ENTRY};

static FILE *file(const char *prefix,const char *suffix)
{
    char name[1024];FILE *f;
    snprintf(name,sizeof name,"%s.%s",prefix,suffix);f=fopen(name,"w");
    if(!f)exit(2);
    return f;
}
static void dump(const char *prefix,const char *suffix,const unsigned char *data)
{
    unsigned i;FILE *f=file(prefix,suffix);
    for(i=0;i<BYTES;i+=16u) {
        int j;for(j=15;j>=0;j--)fprintf(f,"%02x",data[i+(unsigned)j]);fputc('\n',f);
    }
    fclose(f);
}
int main(int argc,char **argv)
{
    gfx_surface_table tab;
    gfx_surface dst={FB,640,480,12,0,0};
    gfx_mem mem={0,memory,BYTES,tex,sizeof tex};
    unsigned strip_h=16;
    assetpak pak;assetpak_limits limit={0x300000u};
    stg_draw_cfg draw;stg_hud_live live={0};stg_frame_stats stats;
    exec_sw_state es;exec_sw_error err;exec_sw_stats xs;
    gfx_list list;FILE *f;unsigned i,off=0,knob=1024;
    size_t size;
    if(argc<3)return 2;
    if(argc>3)knob=(unsigned)atoi(argv[3]);
    if(argc>4)strip_h=(unsigned)atoi(argv[4]);
    if(strip_h!=16 && strip_h!=32)return 2;
    for(i=0;i<BYTES;i++)memory[i]=(unsigned char)(i*37u+(i>>12));
    f=fopen(argv[2],"rb");if(!f)return 3;
    size=fread(memory+SRC,1,0x300000u,f);fclose(f);
    if(assetpak_parse(&pak,memory+SRC,(unsigned)size,&limit))return 4;
    gfx_surface_reset(&tab);
    if(gfx_surface_set(&tab,0,&dst) || gfx_surface_set(&tab,1,&dst))return 5;
    for(i=0;i<13;i++) {
        assetpak_entry e;gfx_surface s;
        if(assetpak_entry_at(&pak,entries[i],&e))return 6;
        s=(gfx_surface){SRC+e.offset,e.width,e.height,e.stride_log2,0,e.format};
        if(i==0 || i==1 || i==5) {
            if(off+e.length>sizeof tex)return 7;
            memcpy(tex+off,memory+SRC+e.offset,e.length);s.base=off;s.space=1;off+=e.length;
        }
        if(gfx_surface_set(&tab,(int)i+2,&s))return 8;
    }
    stg_modes_draw_cfg(&draw,0);
    stg_game_init(&game,STG_GAME_SEED);
    stg_game_press_begin(&game,argc>5 && atoi(argv[5])==8?STG_KIND_SMALL8:STG_KIND_ROUND16);
    stg_game_set_knob(&game,knob);
    for(i=0;i<1200;i++)stg_game_step(&game,0);
    memcpy(initial,memory,BYTES);
    gfx_list_begin(&list,flat,32768,&tab);
    if(stg_game_compose(&game,&list,&draw,&live,&stats))return 9;
    exec_sw_reset(&es);exec_sw_stats_clear(&xs);
    if(exec_sw_run(&es,&tab,&mem,flat,list.used,list.used,&xs,&err))return 10;
    memcpy(golden,memory,BYTES);memcpy(memory,initial,BYTES);
    gfx_instance_arena arena={memory+INSTANCE,INSTANCE,0x80000u,0,1};
    gfx_list_begin(&list,(gfx_cmd *)(void *)(memory+TABLE),32768,&tab);list.instances=&arena;
    if(stg_game_compose(&game,&list,&draw,&live,&stats))return 11;
    memcpy(initial,memory,BYTES);
    gfx_pb_config cfg={{480u/strip_h,strip_h==16?4u:5u,0,0},1024,8192};
    gfx_pb_workspace work={memory+PB,PB,0x20400u,1};
    gfx_instance_view view={arena.host,arena.dev_base,arena.used},pv={memory+PB,PB,work.bytes};
    gfx_pb_error pe;gfx_pb_stats ps;
    if(gfx_pb_build(&tab,list.buf,list.used,&view,1,&cfg,&work,&pe))return 12;
    if(exec_sw_run_pb(&tab,&mem,&pv,&view,1,&cfg,&ps,&pe))return 13;
    if(memcmp(memory+FB,golden+FB,480u*4096u))return 14;
    if(ps.pixels!=xs.pixel_total-xs.src_oob || ps.key_skip!=xs.key_skip)return 15;
    memcpy(golden,memory,BYTES);
    const unsigned *header=(const unsigned *)(const void *)(memory+PB);
    unsigned span=0;for(unsigned z=0;z<cfg.strips.lstrips;z++)span+=(header[17+z*2]*8u+15u)&~15u;
    dump(argv[1],"init",initial);dump(argv[1],"gold",golden);
    f=file(argv[1],"surf");
    for(i=0;i<16;i++)fprintf(f,"%08x\n%08x\n%08x\n",tab.s[i].base,tab.s[i].dim,tab.s[i].ctrl);
    fclose(f);f=file(argv[1],"tex");
    for(i=0;i<4096;i++)fprintf(f,"%08x\n",((unsigned *)(void *)tex)[i]);
    fclose(f);f=file(argv[1],"stats");
    fprintf(f,"%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n",
        xs.pixel_total-xs.src_oob,xs.key_skip,header[4],ps.instances,ps.refs,list.used,512u,header[7],span);fclose(f);
    printf("F5 GAME GOLDEN PASS knob=%u active=%u records=%u pixels=%u key=%u padding=equal\n",
           knob,stats.act.ebullet,list.used,xs.pixel_total-xs.src_oob,xs.key_skip);
    return 0;
}
