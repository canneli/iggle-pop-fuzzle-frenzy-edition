#ifndef IGGLE_CUSTOM_LEVELS_H
#define IGGLE_CUSTOM_LEVELS_H
#include <windows.h>

enum { IGGLE_LEVEL_PAGE_SIZE = 5 };
enum { IGGLE_SLOW_TIMER = 0x4948 };
void iggle_end_slow_power(void);
enum { IGGLE_DIAMOND_LIMIT = 10 };
typedef struct { float x,y,age; } IggleDiamond;
typedef struct {
    IggleDiamond gems[IGGLE_DIAMOND_LIMIT];
    unsigned count,collected;
    float hero_x,hero_y,aura;
} IggleDiamondView;
BOOL iggle_diamond_view(IggleDiamondView *view);
BOOL iggle_custom_directory(wchar_t path[MAX_PATH]);
int iggle_custom_refresh(void);
const char *iggle_custom_name(int index);
BOOL iggle_custom_select(int index);
BOOL iggle_custom_select_path(const wchar_t *path);
BOOL iggle_custom_path_at(int index, wchar_t path[MAX_PATH]);
BOOL iggle_custom_in_game(int index);
BOOL iggle_custom_new_path(wchar_t path[MAX_PATH]);
int iggle_current_language(void);
BOOL iggle_custom_get_path(char path[MAX_PATH]);
BOOL iggle_custom_launch(void *menu);
void iggle_open_custom_levels(void *menu);
void iggle_show_custom_levels(void);
void iggle_leave_custom_level(void);

#endif
