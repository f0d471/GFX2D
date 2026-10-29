/* Content adapter for the existing command-list UI. No board or resource state
 * is exposed. New independent games use api/platform.h. */
#ifndef CONSOLE_VIEWS_H
#define CONSOLE_VIEWS_H
#include "appfw.h"
#include "console_ids.h"
#include "app_port.h"
#include "input.h"
#include "gfx/gfx.h"
enum { CONSOLE_WIDTH=640,CONSOLE_HEIGHT=480,CONSOLE_VIEW_STG=0,CONSOLE_VIEW_DEMO=1 };
struct sw6_scene_b;
int console_view_prepare(appfw_ctx *ctx,int family,int hardware);
int console_view_begin(appfw_ctx *ctx,gfx_list *list,int *destination);
int console_view_submit(appfw_ctx *ctx,const gfx_list *list,int destination,int hardware);
int console_view_flush(appfw_ctx *ctx);
int console_view_backend(appfw_ctx *ctx);
void console_view_set_backend(appfw_ctx *ctx,int backend);
int console_demo_prepare(appfw_ctx *ctx,struct sw6_scene_b *scene);
uint32_t console_view_crc(appfw_ctx *ctx);
uint32_t console_view_fields(void);
uint32_t console_view_presents(void);
uint32_t console_view_faults(void);
#endif
