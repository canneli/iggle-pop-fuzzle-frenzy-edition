#ifndef IGGLE_MENU_H
#define IGGLE_MENU_H
#include <windows.h>
#define IGGLE_MENU_POINTER (WM_APP + 0x48c)
BOOL iggle_menu_load(const char *path);
void iggle_menu_clear(void);
void iggle_menu_use_pointer(void);
int iggle_focus_next(const POINT *points, size_t count, int selected, int direction);
BOOL iggle_menu_move(HWND window, int direction);
BOOL iggle_menu_confirm(HWND window);
BOOL iggle_confirm_native_options(int x,int y);
BOOL iggle_overlay_is_open(void);
BOOL iggle_editor_is_open(void);
#endif
