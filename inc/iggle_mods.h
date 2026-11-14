#ifndef IGGLE_MODS_H
#define IGGLE_MODS_H
#include <windows.h>

enum { IGGLE_MOD_WORLD=1, IGGLE_MOD_HERO=2, IGGLE_MOD_NAME=31 };
typedef struct {
    char id[IGGLE_MOD_NAME];
    char name[IGGLE_MOD_NAME];
    int kind;
    BOOL enabled;
} IggleModInfo;

int iggle_mods_scan(void);
BOOL iggle_mods_info(int index,IggleModInfo *info);
BOOL iggle_mods_toggle(int index);
BOOL iggle_mods_world(char id[IGGLE_MOD_NAME],char name[IGGLE_MOD_NAME]);
void iggle_mods_startup(void);
BOOL iggle_mods_resource(const char *resource,BOOL world,char path[MAX_PATH]);
BOOL iggle_mods_stage(wchar_t path[MAX_PATH]);
BOOL iggle_mods_preview(int tile,char path[MAX_PATH]);
void iggle_open_mods(void *menu);
#endif
