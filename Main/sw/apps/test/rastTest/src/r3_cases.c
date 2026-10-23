#include "r3_cases.h"

uint32_t r3_texel(unsigned word)
{
    unsigned x=word%64u,y=word/64u;
    if((x+y)%5u==0) return R3_KEY;
    return 0xff000000u|((x*3u+40u)<<16)|((y*3u+30u)<<8)|((x+y+60u)&255u);
}

/* Deterministic shared board/RTL inputs; expected pixels come from both R1 references. */
int r3_scene(unsigned frame,unsigned textured,rast_record *out,unsigned capacity)
{
    unsigned n=0;
    if(!out || capacity<R3_SCENE_RECORDS) return -1;
    for(unsigned i=0;i<R3_SCENE_RECORDS;++i) {
        unsigned group=i/2u;
        int x=group ? (int)((group*71u+frame*13u)%540u):0;
        int y=group ? (int)((group*61u+frame*17u)%400u):0;
        int w=64+(int)(frame%3u)*3,h=32+(int)(group%3u)*7;
        uint16_t z=(uint16_t)(8000+group*1700+frame*31);
        /* Near keyed geometry followed by a farther surface: holes must not write Z. */
        if(group<3u) {x=0;y=0;z=(uint16_t)((group==0 ? 10000:group==1 ? 8000:9000)+frame*31);}
        /* Touch the final pixel/bank of the final strip. */
        if(group==7u){x=576;y=464;w=64;h=16;}
        uint32_t c=0xff000000u|((80u+frame*7u)<<16)|((90u+group*11u)<<8)|180u;
        rast_vertex v[3]={{x*16,y*16,z,0,0,c},
            {(x+w)*16,y*16,(uint16_t)(z+500),1008,0,0xffffffffu},
            {x*16,(y+h)*16,(uint16_t)(z+1000),0,1008,0xff80ff40u}};
        if(i%2u) {
            v[0].x=(x+w)*16;v[0].y=(y+h)*16;v[0].u=1008;v[0].v=1008;
            v[1].x=x*16;v[1].y=(y+h)*16;v[1].u=0;v[1].v=1008;
            v[2].x=(x+w)*16;v[2].y=y*16;v[2].u=1008;v[2].v=0;
        }
        unsigned flags=RAST_VCOLOR_EN;
        if(textured) flags|=RAST_TEX_EN|((group%2u) ? RAST_KEY_EN:0u);
        int made=rast_setup_polygon(v,3,flags,out+n,capacity-n,0);
        if(made!=1) return -1;
        n+=(unsigned)made;
    }
    return (int)n;
}
