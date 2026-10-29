#include <stdio.h>
#include "board.h"
#include "gfx_engine.h"
static uint32_t cycles,status;
static unsigned yielded;
uint32_t board_cycles(void) { cycles+=40000000u;return cycles; }
void board_wait_yield(void) { ++yielded; }
void board_wait_engine(void) { board_wait_yield(); }
uint32_t gfx_engine_test_read(uint32_t offset)
{ if(offset==GFX_ID)return GFX_ID_VALUE;if(offset==GFX_STATUS)return status;return 0; }
void gfx_engine_test_write(uint32_t o,uint32_t v) { (void)o;(void)v; }
#define CHECK(c) do { if(!(c)) { printf("FAIL driver line %d\n",__LINE__);return 1; } } while(0)
int main(void)
{
    status=GFX_STATUS_BUSY;cycles=UINT32_MAX-60000000u;
    CHECK(gfx_engine_soft_reset()==GFX_ENG_E_TIMEOUT && yielded==3);
    yielded=0;cycles=0;
    CHECK(gfx_engine_wait(NULL,NULL,NULL)==GFX_ENG_E_TIMEOUT && yielded==3);
    status=0;yielded=0;CHECK(gfx_engine_soft_reset()==0 && yielded==0);
    status=GFX_STATUS_DONE;CHECK(gfx_engine_wait(NULL,NULL,NULL)==0);
    puts("actual driver deadline/wrap/cooperation PASS");return 0;
}
