#ifndef MENU_CURSOR_H
#define MENU_CURSOR_H
#include <stdint.h>
typedef struct { uint32_t selected,first; } menu_cursor;
/* count is the filtered view size, independent of IDs and registry order. */
void menu_cursor_step(menu_cursor *cursor,uint32_t count,uint32_t rows,int up,int down);
#endif
