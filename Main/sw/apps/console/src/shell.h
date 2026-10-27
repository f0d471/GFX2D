/******************************************************************************
* @file shell.h: 游戏机菜单（SW10）
*
* @brief  开机进这里：标题画面 + 纵向列表 + 页脚提示。启动 = appfw_request。
******************************************************************************/
#ifndef SHELL_H
#define SHELL_H

#include "appfw.h"

const appfw_app *shell_app(void);

#endif /* SHELL_H */
