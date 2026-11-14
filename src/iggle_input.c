#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <xinput.h>
#include <math.h>
#include <string.h>
#include "dd.h"
#include "hook.h"
#include "iggle_input.h"
#include "iggle_menu.h"
#include "mouse.h"

typedef DWORD (WINAPI *ReadPad)(DWORD, XINPUT_STATE *);
static ReadPad read_pad;
static BOOL initialized, gameplay;
static HWND input_window;
static unsigned keyboard_bits, controller_bits;
static DWORD retry_at, previous_tick, repeat_at;
static unsigned menu_directions;
static BOOL confirm_held,menu_pointer;
static WORD editor_buttons;
static int controller_index = -1;
static UINT legacy_index = JOYSTICKID1;
static BOOL legacy_connected;
static const int keys[] = {VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,VK_SPACE,'P',VK_ESCAPE,VK_LBUTTON};

static unsigned wasd_bit(int key) {
    switch (key) {case 'W':return 1;case 'S':return 2;case 'A':return 4;case 'D':return 8;default:return 0;}
}
static BOOL focused(HWND window) {return GetForegroundWindow()==window && !IsIconic(window);}
static void send_transition(HWND window, int index, BOOL down) {
    int key=keys[index];
    if (key==VK_LBUTTON) {
        POINT point;real_GetCursorPos(&point);ScreenToClient(window,&point);
        PostMessageA(window,down?WM_LBUTTONDOWN:WM_LBUTTONUP,down?MK_LBUTTON:0,MAKELPARAM(point.x,point.y));
    } else {
        LPARAM detail=1|((LPARAM)MapVirtualKeyA(key,MAPVK_VK_TO_VSC)<<16);
        if (index<4) detail|=1L<<24;
        if (!down) detail|=(LPARAM)0xc0000000;
        PostMessageA(window,down?WM_KEYDOWN:WM_KEYUP,key,detail);
    }
}
static void update_controller(HWND window,unsigned next) {
    unsigned old=controller_bits|keyboard_bits, combined=next|keyboard_bits;
    controller_bits=next;
    for(int i=0;i<8;i++)if((old^combined)&(1u<<i))send_transition(window,i,(combined&(1u<<i))!=0);
}
void iggle_input_set_gameplay(BOOL active) {if(gameplay && !active && input_window)iggle_input_release(input_window);gameplay=active;}
void iggle_input_init(HWND window) {
    if (!window || window!=g_ddraw.hwnd || input_window==window) return;
    input_window=window;
    if(!initialized){
        initialized=TRUE;
        const wchar_t *libraries[]={L"xinput1_4.dll",L"xinput1_3.dll",L"xinput9_1_0.dll"};
        wchar_t directory[MAX_PATH];UINT size=GetSystemDirectoryW(directory,MAX_PATH);
        if(size && size<MAX_PATH-32)for(unsigned i=0;i<3;i++){
            wchar_t path[MAX_PATH];lstrcpyW(path,directory);lstrcatW(path,L"\\");lstrcatW(path,libraries[i]);
            HMODULE library=LoadLibraryW(path);
            if(library){read_pad=(ReadPad)GetProcAddress(library,"XInputGetState");if(read_pad)break;FreeLibrary(library);}
        }
    }
    previous_tick=GetTickCount();SetTimer(window,IGGLE_INPUT_TIMER,16,NULL);
}
void iggle_input_release(HWND window) {
    unsigned old=controller_bits|keyboard_bits;
    keyboard_bits=controller_bits=0;
    menu_directions=0;
    confirm_held=FALSE;
    menu_pointer=FALSE;
    for(int i=0;i<8;i++)if(old&(1u<<i))send_transition(window,i,FALSE);
}
static float axis(SHORT value) {return value<0?(float)value/32768.f:(float)value/32767.f;}
static float legacy_axis(DWORD value,UINT low,UINT high) {
    if(high<=low)return 0;return 2.f*(float)((double)value-low)/(high-low)-1.f;
}
static void menu_input(HWND window, unsigned directions, BOOL confirm, BOOL overlay, DWORD now)
{
    if (directions != menu_directions || (directions && (LONG)(now-repeat_at)>=0)) {
        repeat_at = now + (directions == menu_directions ? 130 : 330);
        if (directions) {
            menu_pointer=FALSE;
            int direction = 0;
            while (!(directions & (1u << direction))) ++direction;
            if (overlay || !iggle_menu_move(window, direction))
                PostMessageA(window, WM_KEYDOWN, keys[direction], 1);
        }
    }
    menu_directions = directions;
    if (confirm && !confirm_held) {
        if (menu_pointer) {
            POINT point;
            real_GetCursorPos(&point);
            real_ScreenToClient(window,&point);
            LPARAM position=MAKELPARAM(point.x,point.y);
            if (!overlay) mouse_lock();
            PostMessageA(window,WM_MOUSEMOVE,0,position);
            PostMessageA(window,WM_LBUTTONDOWN,MK_LBUTTON,position);
            PostMessageA(window,WM_LBUTTONUP,0,position);
        } else if (overlay || !iggle_menu_confirm(window)) {
            PostMessageA(window, WM_KEYDOWN, VK_RETURN, 1);
        }
    }
    confirm_held = confirm;
}

#ifdef IGGLE_CUSTOM_TEST
static unsigned test_gameplay_directions;
void iggle_test_menu_input(HWND window, WPARAM button)
{
    if (button>=16 && button<32) {
        test_gameplay_directions=(unsigned)button&15;
        update_controller(window,test_gameplay_directions);
        return;
    }
    if (button==5) {
        PostMessageA(window,WM_KEYDOWN,'T',1);
        return;
    }
    BOOL overlay=iggle_overlay_is_open();
    menu_input(window,button<4?1u<<button:0,button==4,overlay,GetTickCount());
    menu_input(window,0,FALSE,overlay,GetTickCount());
}
#endif

void iggle_input_poll(HWND window) {
    DWORD now=GetTickCount();float dt=(now-previous_tick)/1000.f;previous_tick=now;
    if(dt>.05f)dt=.05f;
#ifdef IGGLE_CUSTOM_TEST
    if (test_gameplay_directions) {
        update_controller(window,test_gameplay_directions);
        return;
    }
#endif
    if(!focused(window)){iggle_input_release(window);return;}
    float lx=0,ly=0,rx=0,ry=0;WORD buttons=0;BOOL connected=FALSE;BYTE trigger=0;
    XINPUT_STATE state;
    if(read_pad && controller_index>=0){
        connected=read_pad(controller_index,&state)==ERROR_SUCCESS;
        if(!connected){controller_index=-1;retry_at=now+500;}
    }
    if(!connected && (LONG)(now-retry_at)>=0){
        retry_at=now+500;
        if(read_pad)for(DWORD i=0;i<4;i++)if(read_pad(i,&state)==ERROR_SUCCESS){controller_index=(int)i;connected=TRUE;break;}
        if(!connected && !legacy_connected)for(UINT i=0;i<joyGetNumDevs();i++){
            JOYINFOEX probe={sizeof(probe),JOY_RETURNALL};
            if(joyGetPosEx(i,&probe)==JOYERR_NOERROR){legacy_index=i;legacy_connected=TRUE;break;}
        }
    }
    if(connected){
        lx=axis(state.Gamepad.sThumbLX);ly=axis(state.Gamepad.sThumbLY);
        rx=axis(state.Gamepad.sThumbRX);ry=axis(state.Gamepad.sThumbRY);
        buttons=state.Gamepad.wButtons;trigger=state.Gamepad.bRightTrigger;
    } else if(legacy_connected){
        JOYINFOEX info={sizeof(info),JOY_RETURNALL};JOYCAPSA caps;
        if(joyGetPosEx(legacy_index,&info)==JOYERR_NOERROR && joyGetDevCapsA(legacy_index,&caps,sizeof(caps))==JOYERR_NOERROR){
            connected=TRUE;lx=legacy_axis(info.dwXpos,caps.wXmin,caps.wXmax);ly=-legacy_axis(info.dwYpos,caps.wYmin,caps.wYmax);
            if(caps.wCaps&JOYCAPS_HASR)rx=legacy_axis(info.dwRpos,caps.wRmin,caps.wRmax);
            if(caps.wCaps&JOYCAPS_HASU)ry=-legacy_axis(info.dwUpos,caps.wUmin,caps.wUmax);
            if(info.dwButtons&1)buttons|=XINPUT_GAMEPAD_A;
            if(info.dwButtons&2)buttons|=XINPUT_GAMEPAD_B;
            if(info.dwButtons&4)buttons|=XINPUT_GAMEPAD_X;
            if(info.dwButtons&128)buttons|=XINPUT_GAMEPAD_START;
            DWORD pov=info.dwPOV;
            if(pov!=JOY_POVCENTERED){if(pov>=31500||pov<=4500)buttons|=XINPUT_GAMEPAD_DPAD_UP;if(pov>=4500&&pov<=13500)buttons|=XINPUT_GAMEPAD_DPAD_RIGHT;if(pov>=13500&&pov<=22500)buttons|=XINPUT_GAMEPAD_DPAD_DOWN;if(pov>=22500&&pov<=31500)buttons|=XINPUT_GAMEPAD_DPAD_LEFT;}
        }else {legacy_connected=FALSE;retry_at=now+500;}
    }
    unsigned next=0;
    BOOL overlay=iggle_overlay_is_open();
    BOOL menu=overlay || !gameplay;
    if(connected){
        if(ly>.3f || (buttons&XINPUT_GAMEPAD_DPAD_UP))next|=1;
        if(ly<-.3f || (buttons&XINPUT_GAMEPAD_DPAD_DOWN))next|=2;
        if(lx<-.3f || (buttons&XINPUT_GAMEPAD_DPAD_LEFT))next|=4;
        if(lx>.3f || (buttons&XINPUT_GAMEPAD_DPAD_RIGHT))next|=8;
        if(!menu && ((buttons&XINPUT_GAMEPAD_A)||trigger>45))next|=16;
        if(buttons&XINPUT_GAMEPAD_START)next|=32;
        if(buttons&XINPUT_GAMEPAD_B)next|=64;
        if(buttons&XINPUT_GAMEPAD_X)next|=128;
        if(fabsf(rx)>.2f || fabsf(ry)>.2f){
            POINT p;real_GetCursorPos(&p);real_ScreenToClient(window,&p);RECT bounds;GetClientRect(window,&bounds);
            float dx=fabsf(rx)>.2f?copysignf((fabsf(rx)-.2f)/.8f,rx):0;
            float dy=fabsf(ry)>.2f?copysignf((fabsf(ry)-.2f)/.8f,ry):0;
            p.x=max(0,min(bounds.right-1,p.x+(LONG)(dx*850*dt)));p.y=max(0,min(bounds.bottom-1,p.y-(LONG)(dy*850*dt)));
            LPARAM cursor=MAKELPARAM(p.x,p.y);
            real_ClientToScreen(window,&p);real_SetCursorPos(p.x,p.y);
            if (menu) {
                menu_pointer=TRUE;
                iggle_menu_use_pointer();
                PostMessageA(window,WM_MOUSEMOVE,0,cursor);
            }
        }
    }
    if (iggle_editor_is_open()) {
        WORD pressed=buttons & ~editor_buttons;
        if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) PostMessageA(window,WM_KEYDOWN,VK_PRIOR,1);
        if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) PostMessageA(window,WM_KEYDOWN,VK_NEXT,1);
        if (pressed & XINPUT_GAMEPAD_X) PostMessageA(window,WM_KEYDOWN,VK_DELETE,1);
        if (pressed & (XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_Y)) PostMessageA(window,WM_KEYDOWN,'T',1);
        if (pressed & XINPUT_GAMEPAD_RIGHT_THUMB) PostMessageA(window,WM_KEYDOWN,'M',1);
        if (pressed & XINPUT_GAMEPAD_BACK) PostMessageA(window,WM_KEYDOWN,VK_F2,1);
        if (pressed & XINPUT_GAMEPAD_START) PostMessageA(window,WM_KEYDOWN,VK_F5,1);
        next &= ~(32u|128u);
    }
    editor_buttons=buttons;
    if(menu) {
        menu_input(window,next&15,connected && (buttons&XINPUT_GAMEPAD_A),overlay,now);
        next &= ~(15u|128u);
    } else {
        menu_directions=0;
        menu_pointer=FALSE;
        confirm_held=(buttons&XINPUT_GAMEPAD_A)!=0;
    }
    update_controller(window,next);
}
SHORT iggle_input_key(int key) {
    unsigned held=keyboard_bits|controller_bits;
    for(int i=0;i<8;i++)if(keys[i]==key && (held&(1u<<i)))return (SHORT)0x8000;
    return 0;
}
BOOL iggle_input_message(HWND window,UINT *message,WPARAM *key,LPARAM *details) {
    if(*message==WM_KILLFOCUS || (*message==WM_ACTIVATEAPP && !*key))iggle_input_release(window);
    if(!gameplay || !focused(window))return FALSE;
    unsigned bit=wasd_bit((int)*key);
    if(*message==WM_CHAR){int letter=(int)*key;if(letter>='a'&&letter<='z')letter-=32;return wasd_bit(letter)!=0;}
    if(!bit || (*message!=WM_KEYDOWN&&*message!=WM_KEYUP))return FALSE;
    if(*message==WM_KEYDOWN && ((real_GetAsyncKeyState(VK_MENU)|real_GetAsyncKeyState(VK_CONTROL))&0x8000))return FALSE;
    unsigned before=keyboard_bits|controller_bits;
    if(*message==WM_KEYDOWN)keyboard_bits|=bit;else keyboard_bits&=~bit;
    unsigned after=keyboard_bits|controller_bits;
    if((before&bit)==(after&bit))return TRUE;
    int index=bit==1?0:bit==2?1:bit==4?2:3;*key=keys[index];
    *details=1|((LPARAM)MapVirtualKeyA((UINT)*key,MAPVK_VK_TO_VSC)<<16)|(1L<<24);
    if(*message==WM_KEYUP)*details|=(LPARAM)0xc0000000;
    return FALSE;
}
