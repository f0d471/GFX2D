/* Execute an unchanged public SDK consumer, then independently read DDR after
 * cache invalidation. This diagnostic is deliberately outside PUBLIC_APP. */
#include <stdint.h>
#include "board.h"
#include "core/crc32.h"
#include "core/format.h"
#define main sdk_example_main
#if SDK_BOARD_SAMPLE == 1
#include "../../../examples/pixels/main.c"
#define SAMPLE_NAME "pixels"
#else
#include "../../../examples/bounce/main.c"
#define SAMPLE_NAME "bounce"
#endif
#undef main
int main(void)
{
    int result=sdk_example_main();
    char text[192];core_out out;
    core_out_init(&out,text,sizeof text);core_out_str(&out,"SDKCHECK ");core_out_str(&out,SAMPLE_NAME);
    if(result) {
        core_out_str(&out," FAIL rc ");core_out_u32(&out,(uint32_t)result);
        core_out_str(&out,"\r\n");board_puts(text);return result;
    }
    board_dcache_invalidate_all();
    for(unsigned index=0;index<2;index++) {
        crc32_ctx crc;crc32_begin(&crc);
        const uint8_t *pixels=(const uint8_t *)board_fb((int)index);
        for(unsigned y=0;y<FB_HEIGHT;y++)crc32_update(&crc,pixels+y*FB_STRIDE_BYTES,FB_WIDTH*4u);
        core_out_str(&out,index?" crc1 ":" crc0 ");core_out_hex32(&out,crc32_end(&crc));
    }
    core_out_str(&out," swaps ");core_out_u32(&out,board_swap_count());
    core_out_str(&out," PASS\r\n");board_puts(text);return 0;
}
