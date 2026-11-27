#ifndef IGGLE_LEVEL_EDITOR_H
#define IGGLE_LEVEL_EDITOR_H
#include <windows.h>

enum { EDIT_GRID_MAX = 64, EDIT_WORLD_COUNT = 5, EDIT_COLOR_COUNT = 4, EDIT_RAINBOW = 4 };
enum { EDIT_HERO_COUNT = 5 };
enum { EDIT_WALL, EDIT_FLOOR, EDIT_HERO, EDIT_CAGE, EDIT_HOME, EDIT_ZOOG, EDIT_ERASE, EDIT_POWER, EDIT_TOOL_COUNT };
enum { ZOOG_TRACKER, ZOOG_TRACKER_FAST, ZOOG_BLOBBY, ZOOG_BLOBBY_FAST, ZOOG_SHOOTER,
       ZOOG_ARMOR, ZOOG_FIRE, ZOOG_TUNNEL, ZOOG_SUPER, ZOOG_GUARD, ZOOG_GUARD_FAST,
       ZOOG_BOMB_SLOW, ZOOG_BOMB_SLOW_RANDOM, ZOOG_BOMB_FAST, ZOOG_BOMB_FAST_RANDOM, ZOOG_BOMB_HYPER,
       ZOOG_GMZ, ZOOG_DRAGON, EDIT_ZOOG_COUNT };
enum { POWER_FREEZE, POWER_ZAP, POWER_SORT, POWER_POP, POWER_SLOW, EDIT_POWER_COUNT };
enum { EDIT_HAS_FLOOR=1, EDIT_HAS_WALL_H=2, EDIT_HAS_WALL_V=4 };

typedef struct {
    int columns, rows, world, hero;
    unsigned char tiles[EDIT_GRID_MAX+1][EDIT_GRID_MAX+1];
    unsigned char objects[EDIT_GRID_MAX][EDIT_GRID_MAX];
    unsigned char colors[EDIT_GRID_MAX][EDIT_GRID_MAX];
    unsigned char monsters[EDIT_GRID_MAX][EDIT_GRID_MAX];
    unsigned char powers[EDIT_GRID_MAX][EDIT_GRID_MAX];
    unsigned char power_colors[EDIT_GRID_MAX][EDIT_GRID_MAX];
} EditorBoard;

BOOL iggle_editor_open(const wchar_t *path);
void iggle_editor_close(void);
BOOL iggle_editor_save(const wchar_t *path);
BOOL iggle_editor_paint(int x, int y, int side, int tool, int color, int variant);
void iggle_editor_board(EditorBoard *board);
BOOL iggle_editor_dirty(void);
BOOL iggle_editor_set_world(int world);
BOOL iggle_editor_set_hero(int hero);
int iggle_level_hero(const char *path);
const char *iggle_editor_hero_name(int hero);
const char *iggle_level_theme(const char *path);
BOOL iggle_level_prepare(char path[MAX_PATH], char temporary[MAX_PATH]);
#endif
