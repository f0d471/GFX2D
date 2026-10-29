#ifndef CONSOLE_CONTENT_H
#define CONSOLE_CONTENT_H
#include "appfw.h"
#include "stg_runtime.h"
#include "console_ids.h"
typedef struct { stg_runtime runtime;uint32_t seq;int shown; } console_content;
static inline console_content *content(appfw_ctx *c) { return c->user; }
static inline stg_runtime *content_runtime(appfw_ctx *c) { return &content(c)->runtime; }
const appfw_config *console_content_config(void);
#endif
