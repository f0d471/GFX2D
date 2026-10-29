/******************************************************************************
* @file apps_registry.c: 应用注册表（SW10）
*
* @brief  链接期定死的注册顺序就是菜单顺序。加内容 = 各适配器文件导出
*         `const appfw_app g_app_<name>`，这里加一行取地址。
******************************************************************************/

#include "appfw.h"
#include "console_ids.h"

extern const appfw_app g_app_demo;
extern const appfw_app g_app_stg;
extern const appfw_app g_app_press;
extern const appfw_app g_app_r4;
extern const appfw_app g_app_about;

const appfw_app *const g_appfw_registry[] = {
    &g_app_demo,
    &g_app_stg,
    &g_app_press,
    &g_app_r4,
    &g_app_about,
};

const uint32_t g_appfw_registry_n =
    (uint32_t)(sizeof g_appfw_registry / sizeof g_appfw_registry[0]);
