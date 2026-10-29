#include "appfw.h"
extern const appfw_app g_app_demo,g_app_stg,g_app_press,g_app_r4,g_app_about;
const appfw_app *const g_appfw_registry[]={&g_app_demo,&g_app_stg,&g_app_press,&g_app_r4,&g_app_about};
const uint32_t g_appfw_registry_n=5;
