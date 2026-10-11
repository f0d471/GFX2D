#include "board_ddr_log.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    uint8_t bytes[64];board_ddr_log l;
    memset(bytes,0xa5,sizeof bytes);board_ddr_log_init(&l,bytes,sizeof bytes);
    board_ddr_log_append(&l,"123456789");assert((l.crc^0xffffffffu)==0xcbf43926u);
    board_ddr_log_init(&l,bytes,sizeof bytes);
    board_ddr_log_append(&l,"notdone\ndoneish\ndo");assert(!l.flags);
    board_ddr_log_append(&l,"ne\r");assert(!l.flags);
    board_ddr_log_append(&l,"\n");assert(l.flags==BOARD_DDR_LOG_DONE);
    assert(l.used==strlen("notdone\ndoneish\ndone\r\n"));
    assert(!memcmp(bytes,"notdone\ndoneish\ndone\r\n",l.used));
    memset(bytes,0xa5,sizeof bytes);board_ddr_log_init(&l,bytes,3);
    board_ddr_log_append(&l,"done\n");assert(l.used==3&&l.flags==3&&bytes[3]==0xa5);
    puts("F5 DDR LOG PASS exact_bytes/chunked_done/CRC32/overflow_guard");
    return 0;
}
