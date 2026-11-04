/* Native SDL integration: real desktop fullscreen, letterboxing, texture reset,
   focus release and framebuffer identity. Include the host implementation to
   query its actual SDL texture without adding test APIs to the product. */
#include "../../sw/bsp/host/board.c"
#include <assert.h>
#ifdef _WIN32
#include <SDL_syswm.h>
#include <imm.h>
#endif
#undef assert
#define assert(expression) do { if(!(expression)) { \
    fprintf(stderr,"WINDOW CHECK FAIL line %d: %s\n",__LINE__,#expression); \
    fflush(stderr);exit(1); } } while(0)

static void key(SDL_Scancode code,SDL_Keymod mod)
{
    SDL_Event event;memset(&event,0,sizeof event);
    event.type=SDL_KEYDOWN;event.key.state=SDL_PRESSED;
    event.key.windowID=SDL_GetWindowID(s_win);
    event.key.keysym.scancode=code;event.key.keysym.sym=SDL_GetKeyFromScancode(code);
    event.key.keysym.mod=(Uint16)mod;
    assert(SDL_PushEvent(&event)==1);
    (void)board_keys();SDL_Delay(25);(void)board_keys();
}

static uint32_t pixels(void)
{
    uint32_t value=2166136261u,*fb=board_fb(0);
    for(unsigned y=0;y<FB_HEIGHT;++y)for(unsigned x=0;x<FB_WIDTH;++x)
        value=(value^fb[y*FB_ROW_PIXELS+x])*16777619u;
    return value;
}

static void adaptive_frame(void)
{
    int output_w,output_h;board_frame frame;
    assert(!SDL_GetRendererOutputSize(s_ren,&output_w,&output_h));
    assert(!board_frame_acquire(0,1,&frame));
    assert(frame.width==canvas_width_for_output(output_w,output_h));
    assert(frame.height==480 && frame.stride>=frame.width);
    if(frame.width>FB_WIDTH)for(unsigned y=0;y<frame.height;++y)for(unsigned x=0;x<frame.width;++x)
        frame.pixels[(size_t)y*frame.stride+x]=x<frame.width-160u?0x00244868u:0x00050912u;
    uint32_t ticket=0;assert(!board_present_frame_request(0,&frame,&ticket));
    assert(!board_present_wait(ticket));
    board_frame invalid=frame;invalid.stride=frame.width-1u;
    uint32_t swaps=board_swap_count();assert(board_present_frame_request(0,&invalid,&ticket)==-1);
    assert(board_swap_count()==swaps);
    int logical_w,logical_h;float sx,sy;
    SDL_RenderGetLogicalSize(s_ren,&logical_w,&logical_h);SDL_RenderGetScale(s_ren,&sx,&sy);
    assert(logical_w==(int)frame.width && logical_h==480);
    assert(sx>0 && sy>0 && sx-sy<0.001f && sy-sx<0.001f);
    SDL_Event reset;memset(&reset,0,sizeof reset);reset.type=SDL_RENDER_TARGETS_RESET;
    assert(SDL_PushEvent(&reset)==1);(void)board_keys();
    int tw,th;assert(!SDL_QueryTexture(s_tex,0,0,&tw,&th));
    assert(tw==logical_w && th==logical_h);
    printf("WINDOW ADAPTIVE SIZE output=%dx%d canvas=%ux%u scale=%g/%g\n",
           output_w,output_h,frame.width,frame.height,(double)sx,(double)sy);fflush(stdout);
}

static void adaptive_checks(void)
{
    const int sizes[][3]={{1024,768,640},{2560,1600,768},{1920,1080,853},
        {3840,2160,853},{3440,1440,1147},{5120,1440,1707},{7680,2160,1707},
        {8192,1024,1792},{1080,1920,640},{0,0,640}};
    for(unsigned i=0;i<sizeof sizes/sizeof sizes[0];++i)
        assert(canvas_width_for_output(sizes[i][0],sizes[i][1])==(unsigned)sizes[i][2]);
    puts("WINDOW ADAPTIVE POLICY PASS 4:3/16:10/16:9/ultrawide/4K/portrait/limit");
    const int windows[][2]={{640,480},{768,480},{1280,720},{1680,720},{1920,540}};
    for(unsigned i=0;i<sizeof windows/sizeof windows[0];++i) {
        SDL_SetWindowSize(s_win,windows[i][0],windows[i][1]);SDL_Delay(70);(void)board_keys();
        adaptive_frame();
    }
    SDL_setenv("GFX2D_ASPECT","fixed",1);
    board_frame fixed;assert(!board_frame_acquire(0,1,&fixed));assert(fixed.width==640);
    SDL_setenv("GFX2D_ASPECT","auto",1);
    puts("WINDOW ADAPTIVE RESIZE PASS fixed override and bounded buffers");
    int displays=SDL_GetNumVideoDisplays();assert(displays>0);
    for(int i=0;i<displays;++i) {
        SDL_Rect bounds;assert(!SDL_GetDisplayUsableBounds(i,&bounds));
        SDL_SetWindowSize(s_win,960,600);SDL_SetWindowPosition(s_win,bounds.x+40,bounds.y+40);
        SDL_Delay(150);(void)board_keys();
        assert(SDL_GetWindowDisplayIndex(s_win)==i);
        adaptive_frame();key(SDL_SCANCODE_F11,KMOD_NONE);
        assert(SDL_GetWindowFlags(s_win)&SDL_WINDOW_FULLSCREEN);
        adaptive_frame();key(SDL_SCANCODE_ESCAPE,KMOD_NONE);
        assert(!(SDL_GetWindowFlags(s_win)&SDL_WINDOW_FULLSCREEN));
        printf("WINDOW DISPLAY PASS %d bounds=%d,%d,%d,%d\n",i,bounds.x,bounds.y,bounds.w,bounds.h);
    }
    /* A synthetic absent position tests recovery without unplugging user hardware. */
    int x=100000,y=100000,w=640,h=480;
    fit_window_to_display(&x,&y,&w,&h);
    assert(x!=100000 && y!=100000);
    SDL_Event disconnect;memset(&disconnect,0,sizeof disconnect);
    disconnect.type=SDL_DISPLAYEVENT;disconnect.display.event=SDL_DISPLAYEVENT_DISCONNECTED;
    assert(SDL_PushEvent(&disconnect)==1);(void)board_keys();adaptive_frame();
    puts("WINDOW DISPLAY RECOVERY PASS synthetic disconnect and absent-position recovery");
    board_present(0);
}

int main(void)
{
    board_init();
    assert(!SDL_IsTextInputActive());
    board_pointer_capture(1);assert(s_pointer_enabled);
    (void)board_keys();s_pointer_dx=s_pointer_dy=0;
    SDL_Event motion;memset(&motion,0,sizeof motion);motion.type=SDL_MOUSEMOTION;
    motion.motion.xrel=17;motion.motion.yrel=-11;
    assert(SDL_PushEvent(&motion)==1);
    int32_t dx,dy;board_pointer_read(&dx,&dy);
    assert(dx==17 && dy==-11);
    board_pointer_read(&dx,&dy);assert(dx==0 && dy==0);
    SDL_Event lost;memset(&lost,0,sizeof lost);lost.type=SDL_WINDOWEVENT;
    lost.window.event=SDL_WINDOWEVENT_FOCUS_LOST;assert(SDL_PushEvent(&lost)==1);
    board_pointer_read(&dx,&dy);assert(!SDL_GetRelativeMouseMode() && dx==0 && dy==0);
    board_pointer_capture(0);assert(!s_pointer_enabled && !SDL_GetRelativeMouseMode());
    puts("WINDOW POINTER PASS relative motion consumed once, focus-loss release, app-leave release");
#ifdef _WIN32
    if(getenv("GFX2D_WINDOW_FOCUS_ONLY"))goto focus_check;
#endif
    for(unsigned y=0;y<FB_HEIGHT;++y)for(unsigned x=0;x<FB_WIDTH;++x)
        board_fb(0)[y*FB_ROW_PIXELS+x]=(x*3u<<16)^y*17u;
    uint32_t original=pixels();
    int width,height;SDL_GetWindowSize(s_win,&width,&height);
    for(unsigned i=0;i<12;++i) {
        printf("WINDOW STEP fullscreen %u\n",i);fflush(stdout);
        key(i&1u?SDL_SCANCODE_RETURN:SDL_SCANCODE_F11,i&1u?KMOD_ALT:KMOD_NONE);
        assert(SDL_GetWindowFlags(s_win)&SDL_WINDOW_FULLSCREEN);
        key(SDL_SCANCODE_ESCAPE,KMOD_NONE);
        assert(!(SDL_GetWindowFlags(s_win)&SDL_WINDOW_FULLSCREEN));
        int w,h;SDL_GetWindowSize(s_win,&w,&h);assert(w==width && h==height);
    }
    puts("WINDOW FULLSCREEN PASS F11/AltEnter/Escape x12 restore");
    for(unsigned scale=1;scale<=3;++scale) {
        key((SDL_Scancode)(SDL_SCANCODE_F1+scale-1),KMOD_NONE);
        int w,h;SDL_GetWindowSize(s_win,&w,&h);
        assert(w==(int)FB_WIDTH*(int)scale && h==(int)FB_HEIGHT*(int)scale);
    }
    SDL_SetWindowSize(s_win,1000,700);SDL_Delay(30);(void)board_keys();
    int logical_w,logical_h;float sx,sy;
    SDL_RenderGetLogicalSize(s_ren,&logical_w,&logical_h);SDL_RenderGetScale(s_ren,&sx,&sy);
    assert(logical_w==(int)FB_WIDTH && logical_h==(int)FB_HEIGHT);
    assert(sx>0 && sy>0 && sx-sy<0.001f && sy-sx<0.001f);
    puts("WINDOW SCALE PASS F1/F2/F3 and 4:3 logical aspect after arbitrary resize");
    SDL_ScaleMode mode;key(SDL_SCANCODE_F4,KMOD_NONE);
    assert(!SDL_GetTextureScaleMode(s_tex,&mode) && mode==SDL_ScaleModeLinear);
    SDL_Event reset;memset(&reset,0,sizeof reset);reset.type=SDL_RENDER_DEVICE_RESET;
    assert(SDL_PushEvent(&reset)==1);(void)board_keys();
    assert(!SDL_GetTextureScaleMode(s_tex,&mode) && mode==SDL_ScaleModeLinear);
    key(SDL_SCANCODE_F4,KMOD_NONE);
    assert(!SDL_GetTextureScaleMode(s_tex,&mode) && mode==SDL_ScaleModeNearest);
    assert(pixels()==original);
    puts("WINDOW FILTER PASS nearest/linear, renderer reset, framebuffer unchanged");
    adaptive_checks();assert(pixels()==original);
#ifdef _WIN32
focus_check:
    ;
    SDL_SysWMinfo info;SDL_VERSION(&info.version);
    assert(SDL_GetWindowWMInfo(s_win,&info));
    HWND window=info.info.win.window;
    SDL_Window *recipient=SDL_CreateWindow("MYRIAD focus check",100,100,320,240,SDL_WINDOW_HIDDEN);
    assert(recipient);SDL_SysWMinfo other;SDL_VERSION(&other.version);
    assert(SDL_GetWindowWMInfo(recipient,&other));
    SDL_RaiseWindow(s_win);
    DWORD foreground=GetWindowThreadProcessId(GetForegroundWindow(),0),own=GetCurrentThreadId();
    BOOL attached=foreground!=own?AttachThreadInput(own,foreground,TRUE):FALSE;
    ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);SetActiveWindow(window);SetFocus(window);
    if(attached)AttachThreadInput(own,foreground,FALSE);
    SDL_Delay(40);(void)board_keys();
    if(GetForegroundWindow()!=window) {
        fputs("INCOMPLETE Windows did not grant the test window foreground; no key injected\n",stderr);
        return 3;
    }
    board_pointer_capture(1);
    /* Keep the IME actively composing. Merely checking SDL's text flag missed
       the user's actual failure: physical W/S held, SDL bitmap still zero. */
    HIMC ime=ImmGetContext(window);
    BOOL ime_was_open=FALSE;DWORD ime_conversion=0,ime_sentence=0;
    if(ime) {
        ime_was_open=ImmGetOpenStatus(ime);
        ImmGetConversionStatus(ime,&ime_conversion,&ime_sentence);
        ImmSetOpenStatus(ime,TRUE);ImmSetConversionStatus(ime,IME_CMODE_NATIVE,0);
    }
    const WORD movement_vk[4]={'W','A','S','D'};
    const uint32_t movement_bits[4]={BOARD_KEY_UP,BOARD_KEY_LEFT|PLATFORM_KEY_STRAFE_L,
                                    BOARD_KEY_DOWN,BOARD_KEY_RIGHT|PLATFORM_KEY_STRAFE_R};
    for(unsigned i=0;i<4;++i) {
        if(GetForegroundWindow()!=window) {
            fputs("INCOMPLETE foreground changed before native WASD test\n",stderr);return 3;
        }
        INPUT movement;memset(&movement,0,sizeof movement);movement.type=INPUT_KEYBOARD;
        movement.ki.wScan=(WORD)MapVirtualKeyW(movement_vk[i],MAPVK_VK_TO_VSC);
        movement.ki.dwFlags=KEYEVENTF_SCANCODE;assert(SendInput(1,&movement,sizeof movement)==1);
        uint32_t held=0;
        for(unsigned attempt=0;attempt<20 && (held&movement_bits[i])!=movement_bits[i];++attempt) {
            SDL_Delay(10);held=board_keys();
        }
        /* Release even when the observation is wrong. */
        movement.ki.dwFlags|=KEYEVENTF_KEYUP;assert(SendInput(1,&movement,sizeof movement)==1);
        SDL_Delay(30);uint32_t release=board_keys();
        printf("WINDOW WASD key=%c held=%u release=%u layout=%p text=%d\n",
            movement_vk[i],held,release,(void*)GetKeyboardLayout(0),SDL_IsTextInputActive());
        assert((held&movement_bits[i])==movement_bits[i]);assert(!(release&movement_bits[i]));
    }
    if(ime) {
        ImmSetConversionStatus(ime,ime_conversion,ime_sentence);ImmSetOpenStatus(ime,ime_was_open);
        ImmReleaseContext(window,ime);
    }
    puts("WINDOW WASD PASS physical W/A/S/D and release while IME is open");
    INPUT input;memset(&input,0,sizeof input);input.type=INPUT_KEYBOARD;
    input.ki.wScan=(WORD)MapVirtualKeyW('W',MAPVK_VK_TO_VSC);input.ki.dwFlags=KEYEVENTF_SCANCODE;
    assert(SendInput(1,&input,sizeof input)==1);
    uint32_t pressed=0;
    for(unsigned attempt=0;attempt<20 && !(pressed&BOARD_KEY_UP);++attempt) {
        SDL_Delay(10);pressed=board_keys();
    }
    unsigned focus_flags=SDL_GetWindowFlags(s_win);
    int sdl_w=SDL_GetKeyboardState(0)[SDL_SCANCODE_W];
    int native_w=GetAsyncKeyState('W');
    SDL_ShowWindow(recipient);SDL_RaiseWindow(recipient);
    SetForegroundWindow(other.info.win.window);SDL_Delay(25);
    uint32_t released=board_keys();
    /* Always release before checking observations, including failed assertions. */
    input.ki.dwFlags=KEYEVENTF_SCANCODE|KEYEVENTF_KEYUP;
    assert(SendInput(1,&input,sizeof input)==1);SDL_Delay(20);(void)board_keys();
    printf("WINDOW KEY DIAG flags=%u SDL_W=%d native_W=%d pressed=%u released=%u\n",
        focus_flags,sdl_w,native_w,pressed,released);fflush(stdout);
    assert(pressed&BOARD_KEY_UP);assert(released==0);
    SDL_DestroyWindow(recipient);
    puts("WINDOW FOCUS PASS own-window W press and focus-loss release");
#endif
    memset(&reset,0,sizeof reset);reset.type=SDL_QUIT;
    assert(SDL_PushEvent(&reset)==1);(void)board_keys();assert(board_should_quit());
    puts("WINDOW EXIT PASS");SDL_Quit();return 0;
}
