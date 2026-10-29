/* Desktop scheduler port; SDL itself remains in bsp/host. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "core/crc32.h"
#include "console_content.h"
#include "app_port.h"

int main(int argc,char **argv)
{
    uint32_t start=APPFW_MENU,frames=0,step=0,script_keys=0;
    const char *script=NULL;
    int i;
    for(i=1;i<argc;++i) {
        if(strcmp(argv[i],"--help")==0 || strcmp(argv[i],"-h")==0) {
            board_puts("console [--app 0..5] [--frames N] [--input-script frame:keys,...]\n");return 0;
        }
        if(strcmp(argv[i],"--app")==0 && i+1<argc)start=(uint32_t)strtoul(argv[++i],NULL,0);
        else if(strcmp(argv[i],"--frames")==0 && i+1<argc)frames=(uint32_t)strtoul(argv[++i],NULL,0);
        else if(strcmp(argv[i],"--input-script")==0 && i+1<argc)script=argv[++i];
        else { board_puts("console [--app 0..5] [--frames N] [--input-script frame:keys,...]\n");return 2; }
    }
    if(start>APPFW_ABOUT)return 2;
    board_init();board_product_keys(1);crc32_init();
    app_port_system_init();
    if(appfw_init(console_content_config(),start)!=0)return 1;
    while(!board_should_quit() && (!frames || step<frames)) {
        uint32_t keys=app_port_keys();
        while(script && *script) {
            char *end;
            uint32_t at=(uint32_t)strtoul(script,&end,10),value;
            if(*end!=':')return 2;
            value=(uint32_t)strtoul(end+1,&end,0);
            if(at>step)break;
            script_keys=value;script=*end==','?end+1:end;
        }
        if(script) { keys=script_keys;app_port_override_keys(keys); }
        (void)appfw_step(keys);++step;app_port_idle();
    }
    return appfw_shutdown()!=0;
}
