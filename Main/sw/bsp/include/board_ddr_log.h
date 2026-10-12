/* 诊断日志的有界字节 sink；平台负责发布 header 前后的内存屏障。 */
#ifndef BOARD_DDR_LOG_H
#define BOARD_DDR_LOG_H
#include <stdint.h>
/* SHARED尾部[+0x840000,+0xA00000)，避开实例/PB/条带计划工作区/prepass及所有OFFSCREEN测试面。 */
#define BOARD_DDR_LOG_BASE 0x00e40000u
#define BOARD_DDR_LOG_BYTES 0x001c0000u
#define BOARD_DDR_LOG_MAGIC 0x35474f4cu
#define BOARD_DDR_LOG_HEADER 32u
typedef struct {
    volatile uint8_t *data;
    uint32_t capacity,used,flags,crc,line_size;
    char line[4];
} board_ddr_log;
enum { BOARD_DDR_LOG_DONE=1, BOARD_DDR_LOG_OVERFLOW=2 };
static inline void board_ddr_log_init(board_ddr_log *l,volatile uint8_t *data,uint32_t capacity)
{*l=(board_ddr_log){.data=data,.capacity=capacity,.crc=0xffffffffu};}
static inline void board_ddr_log_append(board_ddr_log *l,const char *s)
{
    while(*s){
        const uint8_t c=(uint8_t)*s++;
        if(l->used<l->capacity){
            l->data[l->used++]=c;l->crc^=c;
            for(unsigned i=0;i<8;i++)l->crc=(l->crc>>1)^(0xedb88320u&(0u-(l->crc&1u)));
        }else l->flags|=BOARD_DDR_LOG_OVERFLOW;
        if(c=='\n'){
            if(l->line_size==4&&l->line[0]=='d'&&l->line[1]=='o'&&l->line[2]=='n'&&l->line[3]=='e')
                l->flags|=BOARD_DDR_LOG_DONE;
            l->line_size=0;
        }else if(c!='\r'){
            if(l->line_size<4)l->line[l->line_size]=(char)c;
            if(l->line_size<5)l->line_size++;
        }
    }
}
#endif
