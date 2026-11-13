#include <windows.h>
#include <limits.h>
#include <stdlib.h>
#ifdef IGGLE_CUSTOM_TEST
#include <stdio.h>
#endif
#include "dd.h"
#include "hook.h"
#include "mouse.h"
#include "iggle_menu.h"

struct MenuFocus {
    const char *name;
    const POINT *points;
    size_t count;
};
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#include "iggle_menu_layouts.h"

static const struct MenuFocus *layout;
static int selected = -1;
static BOOL pointer_mode;
void iggle_menu_use_pointer(void) { pointer_mode=TRUE; }

void iggle_menu_clear(void)
{
    layout = NULL;
    selected = -1;
    pointer_mode=FALSE;
}

BOOL iggle_menu_load(const char *path)
{
    const char *file = path;
    for (const char *c = path; *c; ++c)
        if (*c == '/' || *c == '\\') file = c + 1;
    const char *key = file;
    size_t length = strlen(file);
    if (_stricmp(file, "baseLayout.xml") == 0 &&
        (_strnicmp(path, "res/themes/editor/", 18) == 0 ||
         _strnicmp(path, "res\\themes\\editor\\", 18) == 0)) {
        key = "editor/baselayout";
        length = strlen(key);
    } else if (length > 4 && _stricmp(file + length - 4, ".xml") == 0) {
        length -= 4;
    }
    for (size_t i = 0; i < ARRAY_SIZE(menu_layouts); ++i) {
        if (strlen(menu_layouts[i].name) == length &&
            _strnicmp(menu_layouts[i].name, key, length) == 0) {
            layout = &menu_layouts[i];
            selected = -1;
            pointer_mode=FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

static LPARAM point_for_window(HWND window)
{
    POINT point = layout->points[selected];
#ifdef IGGLE_CUSTOM_TEST
    FILE *trace=fopen("custom-smoke.log","a");
    if(trace) {fprintf(trace,"focus %s %ld %ld\n",layout->name,point.x,point.y);fclose(trace);}
#endif
    POINT screen={g_ddraw.mouse.x_adjust+(LONG)(point.x*g_ddraw.mouse.scale_x+.5f),
                  g_ddraw.mouse.y_adjust+(LONG)(point.y*g_ddraw.mouse.scale_y+.5f)};
    real_ClientToScreen(window,&screen);
    real_SetCursorPos(screen.x,screen.y);
    return MAKELPARAM(point.x,point.y);
}

int iggle_focus_next(const POINT *points, size_t count, int selected, int direction)
{
    if (!count) return -1;
    if (selected < 0) return 0;
    POINT origin=points[selected];
    long best_score=LONG_MAX;
    int best=-1;
    for (size_t i=0;i<count;++i) {
        POINT candidate=points[i];
        long dx=candidate.x-origin.x,dy=candidate.y-origin.y;
        long forward=direction==0?-dy:direction==1?dy:direction==2?-dx:dx;
        long across=direction<2?dx:dy;
        if (forward<=0) continue;
        long score=3*labs(across)+forward;
        if (score<best_score) {best_score=score;best=(int)i;}
    }
    return best;
}

BOOL iggle_menu_move(HWND window, int direction)
{
    if (!layout || !layout->count) return FALSE;
    if (pointer_mode) {
        pointer_mode=FALSE;
        selected=-1;
    }
    int next=iggle_focus_next(layout->points,layout->count,selected,direction);
    if (next>=0) selected=next;
    PostMessageA(window, IGGLE_MENU_POINTER, FALSE, point_for_window(window));
    return TRUE;
}

BOOL iggle_menu_confirm(HWND window)
{
    if (pointer_mode || !layout || !layout->count) return FALSE;
    if (selected < 0) selected = 0;
    LPARAM position=point_for_window(window);
    WPARAM action=_stricmp(layout->name,"optionmenu")==0?2:1;
    PostMessageA(window,IGGLE_MENU_POINTER,action,position);
    return TRUE;
}
