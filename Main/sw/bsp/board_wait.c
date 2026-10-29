/* Optional scheduler cooperation. Bare-metal callers retain the no-op default. */
#include "board.h"

static void (*s_wait_hook)(void);
static void (*s_engine_hook)(void);

void board_set_wait_hook(void (*hook)(void))
{
    s_wait_hook = hook;
}

void board_wait_yield(void)
{
    if (s_wait_hook != NULL)
        s_wait_hook();
}

void board_set_engine_wait_hook(void (*hook)(void)) { s_engine_hook=hook; }
void board_wait_engine(void)
{
    if(s_engine_hook)s_engine_hook();
    else board_wait_yield();
}
