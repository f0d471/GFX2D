#ifndef APPFW_H
#define APPFW_H
#include <stdint.h>
enum { APPFW_FRAME_OK=0,APPFW_FRAME_IDLE=1 };
typedef struct appfw_ctx { void *user,*log;uint32_t fps_milli; } appfw_ctx;
typedef struct appfw_app {
    uint32_t id;const char *name,*desc;
    int (*enter)(appfw_ctx *);int (*leave)(appfw_ctx *);
    int (*frame)(appfw_ctx *,uint32_t keys,uint32_t frame);
} appfw_app;
typedef struct { const char *backend;uint32_t scene,ladder; } appfw_detail;
typedef struct {
    void *user;const appfw_app *menu;const appfw_app *const *apps;uint32_t count;
    uint32_t system_mask,transition_mask;
    int (*boot)(appfw_ctx *);int (*prepare)(appfw_ctx *);
    int (*recover)(appfw_ctx *);void (*reset)(appfw_ctx *);
    int (*available)(void *,const appfw_app *);
    void (*describe)(appfw_ctx *,const appfw_app *,appfw_detail *);
    uint32_t (*navigate)(uint32_t keys,uint32_t pressed,uint32_t previous);
} appfw_config;
/* One manager owner per firmware; configure only at initialization. */
const appfw_app *const *appfw_apps(uint32_t *count);
const appfw_app *appfw_app_at(uint32_t index);
const appfw_app *appfw_find(const char *name);
const appfw_app *appfw_app_id(uint32_t id);
int appfw_available(const appfw_app *app);
void appfw_request(const appfw_app *app);
int appfw_init(const appfw_config *config,uint32_t start_id);
int appfw_step(uint32_t keys);
int appfw_shutdown(void);
int appfw_in_shell(void);
uint32_t appfw_input_held(void);
uint32_t appfw_errors(void);
uint32_t appfw_active_id(void);
void appfw_log(const char *line);
int appfw_error(const char *detail,int code);
#endif
