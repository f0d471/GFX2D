#include "../../../sw/bsp/host/board.c"
#undef near
#include "../../../sw/apps/game/gfx2dGame/src/sh_scene.c"
#include <imm.h>

static void send_key(WORD vk,int up)
{
    INPUT input;memset(&input,0,sizeof input);input.type=INPUT_KEYBOARD;
    input.ki.wScan=(WORD)MapVirtualKeyW(vk,MAPVK_VK_TO_VSC);
    input.ki.dwFlags=KEYEVENTF_SCANCODE|(up?KEYEVENTF_KEYUP:0);
    if(SendInput(1,&input,sizeof input)!=1)exit(2);
}
int main(void)
{
    board_init();board_product_keys(1);sh_world_init(42);board_pointer_capture(1);
    SDL_SysWMinfo info;SDL_VERSION(&info.version);SDL_GetWindowWMInfo(s_win,&info);
    HWND win=info.info.win.window;
    DWORD fg=GetWindowThreadProcessId(GetForegroundWindow(),0),own=GetCurrentThreadId();
    BOOL attach=fg!=own?AttachThreadInput(own,fg,TRUE):FALSE;
    ShowWindow(win,SW_RESTORE);SetForegroundWindow(win);SetActiveWindow(win);SetFocus(win);
    if(attach)AttachThreadInput(own,fg,FALSE);
    SDL_Delay(80);(void)board_keys();
    if(GetForegroundWindow()!=win){puts("INCOMPLETE foreground unavailable");return 3;}
    HIMC context=ImmGetContext(win);
    if(context){ImmSetOpenStatus(context,TRUE);ImmSetConversionStatus(context,IME_CMODE_NATIVE,0);ImmReleaseContext(win,context);}
    printf("layout=%p text_active=%d\n",(void*)GetKeyboardLayout(0),SDL_IsTextInputActive());
    const WORD keys[4]={'W','A','S','D'};
    for(unsigned i=0;i<4;++i) {
        sh_world_init(42);(void)board_keys();
        if(GetForegroundWindow()!=win){puts("INCOMPLETE focus changed");return 3;}
        send_key(keys[i],0);SDL_Delay(70);
        uint32_t bits=board_keys();int native=GetAsyncKeyState(keys[i]);
        uint32_t failure=0;for(unsigned k=0;k<6;++k)sh_world_tick(bits,&failure);
        send_key(keys[i],1);SDL_Delay(30);(void)board_keys();
        printf("KEY %c bits=%u native=%d position=%d,%d\n",keys[i],bits,native,s_px,s_pz);
    }
    board_pointer_capture(0);SDL_Quit();return 0;
}
