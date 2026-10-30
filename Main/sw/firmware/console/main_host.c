/* Desktop scheduler port; SDL itself remains in bsp/host. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "core/crc32.h"
#include "console_content.h"
#include "app_port.h"
#include "console_catalog.h"
#include <errno.h>
#include <stdio.h>

static int number(const char *s,uint32_t *value)
{
    char *end;unsigned long v;
    if(!*s || *s=='-')return -1;
    errno=0;v=strtoul(s,&end,0);
    if(errno || *end || v>UINT32_MAX)return -1;
    *value=(uint32_t)v;return 0;
}
static void usage(void)
{
    puts("console [--app ID|KEY] [--list-apps] [--frames N] [--input-script frame:keys,...]");
}

int main(int argc,char **argv)
{
    uint32_t start=APPFW_MENU,frames=0,step=0,script_keys=0;
    const char *script=NULL;
    int i,list=0;
    const char *start_name=NULL;
    for(i=1;i<argc;++i) {
        if(strcmp(argv[i],"--help")==0 || strcmp(argv[i],"-h")==0) {
            usage();return 0;
        }
        if(strcmp(argv[i],"--app")==0 && i+1<argc)start_name=argv[++i];
        else if(strcmp(argv[i],"--list-apps")==0)list=1;
        else if(strcmp(argv[i],"--frames")==0 && i+1<argc) { if(number(argv[++i],&frames)!=0)return 2; }
        else if(strcmp(argv[i],"--input-script")==0 && i+1<argc)script=argv[++i];
        else { usage();return 2; }
    }
    if(list) {
        printf("profile %s catalog %s\n",g_console_profile,g_console_catalog_sha256);
        for(uint32_t n=0;n<g_appfw_registry_n;n++) {
            const console_app_meta *m=&g_console_catalog[n];
            printf("app %u %s %s %s\n",m->app->id,m->app->name,m->kind,m->graphics);
        }
        return 0;
    }
    if(start_name) {
        int found=strcmp(start_name,"menu")==0 || strcmp(start_name,"0")==0;
        for(uint32_t n=0;n<g_appfw_registry_n;n++) {
            const console_app_meta *m=&g_console_catalog[n];uint32_t id;
            if(strcmp(start_name,m->app->name)==0 || (number(start_name,&id)==0 && id==m->app->id)) {
                if(!m->host) { fprintf(stderr,"Application unavailable on host: %s\n",start_name);return 2; }
                start=m->app->id;found=1;break;
            }
        }
        if(!found) { fprintf(stderr,"Unknown application: %s\n",start_name);return 2; }
    }
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
