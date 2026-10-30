#ifndef CONSOLE_CATALOG_H
#define CONSOLE_CATALOG_H
#include "appfw.h"
typedef struct {
    const appfw_app *app;
    uint32_t target_caps,host,shortcut;
    const char *kind,*graphics;
    void (*describe)(appfw_ctx *,appfw_detail *);
} console_app_meta;
static inline int console_app_supported(const console_app_meta *m,int hardware,uint32_t caps)
{ return hardware?(caps&m->target_caps)==m->target_caps:(int)m->host; }
extern const console_app_meta g_console_catalog[];
extern const appfw_app *const g_appfw_registry[];
extern const uint32_t g_appfw_registry_n;
extern const char g_console_profile[],g_console_catalog_sha256[];
const console_app_meta *console_app_metadata(uint32_t id);
void console_modules_boot(appfw_ctx *ctx);
uint32_t console_shortcut(uint32_t shortcut);
void console_catalog_report(void);
#endif
