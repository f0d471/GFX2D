/* Real platform service against an independently controlled display device. */
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "board.h"
static uint32_t pixels[2][64],swaps,keys;
static uint64_t now;
static int quit,never,refuse,no_swap,pending=-1;
void board_init(void) {}
void board_product_keys(int on) { (void)on; }
void board_input_window_policy(int on) { (void)on; }
int board_frame_acquire(int i,int adaptive,board_frame *p)
{ (void)adaptive;*p=(board_frame){pixels[i],8,8,8};return 0; }
int board_present_frame_request(int i,const board_frame *p,uint32_t *ticket)
{ (void)p;if(refuse)return -1;*ticket=swaps;pending=i;return 0; }
int board_present_finished(uint32_t ticket)
{ if(never)return 0;if(no_swap)return 1;if(pending>=0) { ++swaps;pending=-1; }return ticket!=swaps; }
uint32_t board_swap_count(void) { return swaps; }
uint32_t board_keys(void) { return keys; }
uint64_t board_time_ms(void) { return now++; }
int board_should_quit(void) { return quit; }
void board_wait_yield(void) { ++now; }
void board_cpu_write_complete(void) {}
void board_dcache_invalidate_range(const void *p,uint32_t n) { (void)p;(void)n; }
void board_puts(const char *s) { (void)s; }
int board_asset_read(uint32_t off,void *dst,uint32_t n) { (void)off;memset(dst,0x5a,n);return 0; }
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d\n",__LINE__);return 1; } } while(0)
int main(int argc,char **argv)
{
    platform_frame a,b,c;platform_pixels p;char data[4];
    CHECK(platform_frame_acquire(&a)==PLATFORM_INVALID);
    CHECK(platform_init()==0 && platform_init()==PLATFORM_BUSY);
    if(argc>1 && strcmp(argv[1],"closed")==0) {
        quit=1;CHECK(platform_frame_acquire(&a)==PLATFORM_CLOSED);
        puts("platform closed PASS");return 0;
    }
    CHECK(platform_frame_acquire(&a)==0 && platform_frame_acquire(&b)==0);
    CHECK(platform_frame_acquire(&c)==PLATFORM_BUSY);
    CHECK(platform_frame_map(a,&p)==0 && p.width==8 && p.pitch==8);
    CHECK(platform_frame_map(a,&p)==PLATFORM_INVALID);
    CHECK(platform_frame_release(a)==PLATFORM_INVALID && platform_frame_present(a)==PLATFORM_INVALID);
    p.pixels[0]=0x12345678;CHECK(platform_frame_unmap(a)==0);
    if(argc>1 && strcmp(argv[1],"rejected")==0) {
        refuse=1;CHECK(platform_frame_present(a)==PLATFORM_IO);
        CHECK(platform_frame_release(a)==0 && platform_frame_release(b)==0);
        CHECK(platform_shutdown(10)==0 && swaps==0);puts("platform rejected PASS");return 0;
    }
    CHECK(platform_frame_present(a)==0 && platform_frame_present(b)==PLATFORM_BUSY);
    CHECK(platform_frame_map(a,&p)==PLATFORM_INVALID && platform_frame_release(a)==PLATFORM_INVALID);
    if(argc>1 && strcmp(argv[1],"no-swap")==0) {
        no_swap=1;CHECK(platform_frame_wait(a,10)==PLATFORM_IO);
        CHECK(platform_frame_acquire(&c)==PLATFORM_IO && swaps==0);
        puts("platform no-swap rejects fake completion PASS");return 0;
    }
    if(argc>1 && strcmp(argv[1],"timeout")==0) {
        never=1;CHECK(platform_frame_wait(a,3)==PLATFORM_TIMEOUT);
        CHECK(platform_frame_acquire(&c)==PLATFORM_IO && platform_shutdown(10)==PLATFORM_IO);
        CHECK(swaps==0);puts("platform timeout preserves ownership PASS");return 0;
    }
    CHECK(platform_frame_wait(a,10)==0 && swaps==1);
    CHECK(platform_frame_map(a,&p)==PLATFORM_INVALID);
    CHECK(platform_frame_present(b)==0 && platform_frame_wait(b,10)==0 && swaps==2);
    CHECK(platform_frame_acquire(&c)==0 && c.slot==a.slot && c.generation!=a.generation);
    CHECK(platform_frame_map(a,&p)==PLATFORM_INVALID && platform_shutdown(10)==PLATFORM_BUSY);
    CHECK(platform_frame_release(c)==0);
    keys=PLATFORM_KEY_UP;CHECK(platform_input_poll().pressed==PLATFORM_KEY_UP);
    CHECK(platform_input_poll().pressed==0);keys=0;CHECK(platform_input_poll().released==PLATFORM_KEY_UP);
    CHECK(platform_resource_read(BOARD_ASSET_MAX_BYTES-2,data,4)==PLATFORM_RANGE);
    CHECK(platform_resource_read(0,data,4)==0 && data[0]==0x5a);
    CHECK(platform_sleep_ms(5)==0 && platform_shutdown(10)==0);
    CHECK(platform_frame_acquire(&c)==PLATFORM_CLOSED);
    puts("platform ownership/generation/input/resource PASS");return 0;
}
