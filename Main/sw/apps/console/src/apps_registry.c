/******************************************************************************
* @file apps_registry.c: 应用注册表（SW10）
*
* @brief  catalog.json 选择内容，生成注册顺序与元数据；本文件只提供查询。
*         菜单位置、稳定 ID 和快捷键分别表示，不在这里维护第二份应用名单。
******************************************************************************/

#include "appfw.h"
#include <stddef.h>
#include "console_catalog.h"
#include "console_catalog.inc"

const console_app_meta *console_app_metadata(uint32_t id)
{
    for(uint32_t i=0;i<g_appfw_registry_n;i++)
        if(g_console_catalog[i].app->id==id)return &g_console_catalog[i];
    return NULL;
}
uint32_t console_shortcut(uint32_t shortcut)
{
    for(uint32_t i=0;i<g_appfw_registry_n;i++)
        if(g_console_catalog[i].shortcut==shortcut && appfw_available(g_console_catalog[i].app))
            return g_console_catalog[i].app->id;
    return UINT32_MAX;
}
