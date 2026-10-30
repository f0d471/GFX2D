#include "menu_cursor.h"
void menu_cursor_step(menu_cursor *c,uint32_t count,uint32_t rows,int up,int down)
{
    if(!count || !rows) { c->selected=c->first=0;return; }
    if(c->selected>=count)c->selected=0;
    if(up)c->selected=c->selected?c->selected-1:count-1;
    if(down)c->selected=c->selected+1==count?0:c->selected+1;
    if(c->first>c->selected)c->first=c->selected;
    if(c->selected-c->first>=rows)c->first=c->selected-(rows-1);
    if(count<=rows)c->first=0;
    else if(c->first>count-rows)c->first=count-rows;
}
