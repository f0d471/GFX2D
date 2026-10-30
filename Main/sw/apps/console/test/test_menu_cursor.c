#include <stdio.h>
#include <stdint.h>
#include "menu_cursor.h"
#include "console_catalog.h"
static unsigned fails;
#define CHECK(c) do { if(!(c)) { ++fails;printf("FAIL line %u\n",(unsigned)__LINE__); } } while(0)
int main(void)
{
    const uint32_t counts[]={0,1,3,6,7,20,100,UINT32_MAX};
    for(unsigned n=0;n<sizeof counts/sizeof counts[0];n++) {
        menu_cursor c={0,0};uint32_t count=counts[n],position=0;
        for(unsigned step=0;step<1200;step++) {
            int up=step%3==0,down=step%5==0;
            if(count) {
                if(up)position=position?position-1:count-1;
                if(down)position=position+1==count?0:position+1;
            }
            menu_cursor_step(&c,count,6,up,down);
            CHECK(c.selected==position);
            CHECK(!count || (c.first<=c.selected && c.selected-c.first<6 && c.selected<count));
            CHECK(count || (!c.selected && !c.first));
        }
    }
    menu_cursor c={UINT32_MAX,UINT32_MAX};menu_cursor_step(&c,3,6,0,0);
    CHECK(!c.selected && !c.first);
    /* Two unrelated 3D applications declare different needs. The tag has no
       implicit R4 meaning; extra PB capability is neither required nor banned. */
    console_app_meta a={.target_caps=0xc040,.host=1,.graphics="3d"};
    console_app_meta b={.target_caps=0x0040,.host=1,.graphics="3d"};
    CHECK(!console_app_supported(&a,1,0x040f));
    CHECK(!console_app_supported(&a,1,0x346f));
    CHECK(console_app_supported(&a,1,0xd46f));
    CHECK(console_app_supported(&b,1,0x146f));
    CHECK(console_app_supported(&b,1,0x346f));
    CHECK(console_app_supported(&a,0,0));
    b.target_caps=0;CHECK(console_app_supported(&b,1,0x040f));
    b.host=0;CHECK(!console_app_supported(&b,0,0));
    printf("menu/capability fail %u\n",fails);return fails?1:0;
}
