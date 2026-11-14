#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "iggle_mods.h"
#include "config.h"

enum { MOD_LIMIT=32, FILE_LIMIT=64, PATH_LIMIT=72, MOD_BYTES=12*1024*1024 };
static const char *const world_files[]={
    "stage.xml","themebg.jpg","hwallsegment.jpg","_hwallsegment.png",
    "vwallsegment.jpg","_vwallsegment.png","wallpost.jpg","_wallpost.png",
    "filledtile.jpg","_filledtile.png","heart.jpg","_heart.png",
    "swirl.jpg","_swirl.png","floor.rgba","filled.rgba","wallh.rgba","wallv.rgba"
};
static const char *const hero_files[]={
    "fizzynorth0.jpg","_fizzynorth0.png","fizzysouth0.jpg","_fizzysouth0.png",
    "fizzyeast0.jpg","_fizzyeast0.png","fizzywest0.jpg","_fizzywest0.png"
};
typedef struct {
    IggleModInfo info;
    char archive[MAX_PATH];
} Mod;
typedef struct {
    char path[PATH_LIMIT];
    uint32_t size, offset;
} ModFile;
static Mod mods[MOD_LIMIT];
static int count;
static char active[3][IGGLE_MOD_NAME], mounted[3][MAX_PATH];
static char cache[MAX_PATH], settings[MAX_PATH];
static BOOL initialized;
static SRWLOCK mods_lock=SRWLOCK_INIT;

static BOOL valid_id(const char *id)
{
    for (const char *c=id; *c; ++c)
        if (!(*c>='a' && *c<='z') && !(*c>='0' && *c<='9') && *c!='-') return FALSE;
    return *id!=0;
}

static BOOL parse(FILE *input, IggleModInfo *info, ModFile files[FILE_LIMIT], uint32_t *entries)
{
    char magic[8];
    BYTE kind;
    if (fread(magic,1,sizeof magic,input)!=sizeof magic || memcmp(magic,"IGGLEMOD",8) ||
        fread(&kind,1,1,input)!=1 || kind<IGGLE_MOD_WORLD || kind>IGGLE_MOD_HERO ||
        fread(info->id,1,IGGLE_MOD_NAME,input)!=IGGLE_MOD_NAME ||
        fread(info->name,1,IGGLE_MOD_NAME,input)!=IGGLE_MOD_NAME ||
        fread(entries,4,1,input)!=1 || !*entries || *entries>FILE_LIMIT ||
        !memchr(info->id,0,IGGLE_MOD_NAME) || !memchr(info->name,0,IGGLE_MOD_NAME) ||
        !valid_id(info->id) || !info->name[0]) return FALSE;
    for (const unsigned char *c=(const unsigned char *)info->name;*c;++c)
        if (*c<32 || *c>126) return FALSE;
    info->kind=kind;
    if (fseek(input,0,SEEK_END)) return FALSE;
    long size=ftell(input);
    if (size<75 || size>MOD_BYTES || fseek(input,75,SEEK_SET)) return FALSE;
    const char *const *allowed=kind==IGGLE_MOD_WORLD?world_files:hero_files;
    unsigned required=kind==IGGLE_MOD_WORLD?ARRAYSIZE(world_files):ARRAYSIZE(hero_files);
    uint64_t seen=0;
    for (unsigned i=0;i<*entries;++i) {
        ModFile *file=&files[i];
        if (fread(file->path,1,PATH_LIMIT,input)!=PATH_LIMIT ||
            !memchr(file->path,0,PATH_LIMIT) ||
            fread(&file->size,4,1,input)!=1 || !file->size || file->size>MOD_BYTES)
            return FALSE;
        unsigned index=0;
        while (index<required && strcmp(file->path,allowed[index])) ++index;
        if (index==required || (seen & (UINT64_C(1)<<index))) return FALSE;
        seen |= UINT64_C(1)<<index;
        file->offset=ftell(input);
        if ((uint64_t)file->offset+file->size>(uint64_t)size || fseek(input,file->size,SEEK_CUR))
            return FALSE;
    }
    return ftell(input)==size && seen==((UINT64_C(1)<<required)-1);
}

static BOOL path_join(char result[MAX_PATH],const char *dir,const char *name)
{
    int n=snprintf(result,MAX_PATH,"%s\\%s",dir,name);
    return n>0 && n<MAX_PATH;
}

static BOOL write_world_theme(const char *folder,const char *id)
{
    char path[MAX_PATH];
    if (!path_join(path,folder,"theme.xml")) return FALSE;
    FILE *output=fopen(path,"wb");
    if (!output) return FALSE;
    fprintf(output,"<theme name=\"igglemod-%s\" basePath=\"res/igglemods/world/%s\">\n",id,id);
    static const char *const sprites[][2]={
        {"hWallSegment","hWallSegment.jpg"},{"vWallSegment","vWallSegment.jpg"},
        {"filledTile","filledTile.jpg"},{"wallPost","wallPost.jpg"},{"themeBg","themeBg.jpg"},
        {"floorDeco1","heart.jpg"},{"floorDeco2","swirl.jpg"},
        {"cornerDecoLL","heart.jpg"},{"cornerDecoR","swirl.jpg"}
    };
    for (unsigned i=0;i<ARRAYSIZE(sprites);++i)
        fprintf(output,"<image name=\"%s\" file=\"%s\" />\n",sprites[i][0],sprites[i][1]);
    for (int i=1;i<=14;++i)
        fprintf(output,"<image name=\"wallDeco%d\" file=\"%s.jpg\" />\n",i,i%2?"heart":"swirl");
    fputs("<image name=\"hWallSecretReveal\" fullPath=\"res/Themes/Editor/hWallSecretReveal.jpg\" />\n"
          "<image name=\"vWallSecretReveal\" fullPath=\"res/Themes/Editor/vWallSecretReveal.jpg\" />\n"
          "<particleEffect name=\"themeFx\" fullPath=\"res/themes/Editor/fx/themeFx.xml\" />\n"
          "<particleEffect name=\"secretRevealFx\" fullPath=\"res/themes/Editor/fx/secretRevealFx.xml\" />\n",output);
    for (int i=2;i<=5;++i)
        fprintf(output,"<music name=\"musicLoop%d\" fullPath=\"res/sounds/music/musicLoop%d.ogg\" vol=\"0.6\" />\n",i,i);
    fputs("</theme>\n",output);
    BOOL ok=!ferror(output);
    return fclose(output)==0 && ok;
}

static BOOL mount(const Mod *mod, char folder[MAX_PATH])
{
    FILE *input=fopen(mod->archive,"rb");
    if (!input) return FALSE;
    ModFile files[FILE_LIMIT];
    IggleModInfo parsed={0};
    uint32_t entries=0;
    BOOL ok=parse(input,&parsed,files,&entries) &&
        parsed.kind==mod->info.kind && !strcmp(parsed.id,mod->info.id);
    char name[58], target[MAX_PATH];
    static unsigned generation;
    snprintf(name,sizeof name,"%s-%u",mod->info.id,++generation);
    if (!ok || !path_join(folder,cache,name) || !CreateDirectoryA(folder,NULL)) {
        fclose(input);
        return FALSE;
    }
    for (unsigned i=0;i<entries && ok;++i) {
        ModFile *file=&files[i];
        ok=path_join(target,folder,file->path) && !fseek(input,file->offset,SEEK_SET);
        FILE *output=ok?fopen(target,"wb"):NULL;
        ok=output!=NULL;
        BYTE block[4096];
        for (uint32_t remaining=file->size;ok && remaining;) {
            size_t n=min(remaining,sizeof block);
            ok=fread(block,1,n,input)==n && fwrite(block,1,n,output)==n;
            remaining-=(uint32_t)n;
        }
        if (output && fclose(output)) ok=FALSE;
    }
    fclose(input);
    if (ok && parsed.kind==IGGLE_MOD_WORLD) ok=write_world_theme(folder,parsed.id);
    if (!ok) {
        for (unsigned i=0;i<entries;++i)
            if (path_join(target,folder,files[i].path)) DeleteFileA(target);
        if (path_join(target,folder,"theme.xml")) DeleteFileA(target);
        RemoveDirectoryA(folder);
    }
    return ok;
}

static int by_name(const void *a,const void *b)
{
    const Mod *x=a,*y=b;
    int type=x->info.kind-y->info.kind;
    return type?type:_stricmp(x->info.name,y->info.name);
}

int iggle_mods_scan(void)
{
    char directory[MAX_PATH], wildcard[MAX_PATH];
    AcquireSRWLockExclusive(&mods_lock);
    count=0;
    if (!path_join(directory,g_config.game_path,"mods") || !path_join(wildcard,directory,"*.igglemod")) {
        ReleaseSRWLockExclusive(&mods_lock);
        return 0;
    }
    WIN32_FIND_DATAA data;
    HANDLE search=FindFirstFileA(wildcard,&data);
    if (search!=INVALID_HANDLE_VALUE) {
        do {
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            Mod *entry=&mods[count];
            if (!path_join(entry->archive,directory,data.cFileName)) continue;
            FILE *input=fopen(entry->archive,"rb");
            if (!input) continue;
            ModFile files[FILE_LIMIT];
            uint32_t entries;
            BOOL valid=parse(input,&entry->info,files,&entries);
            fclose(input);
            if (!valid) continue;
            BOOL duplicate=FALSE;
            for (int i=0;i<count;++i)
                duplicate|=mods[i].info.kind==entry->info.kind && !strcmp(mods[i].info.id,entry->info.id);
            if (!duplicate) ++count;
        } while (count<MOD_LIMIT && FindNextFileA(search,&data));
        FindClose(search);
    }
    qsort(mods,count,sizeof *mods,by_name);
    int result=count;
    ReleaseSRWLockExclusive(&mods_lock);
    return result;
}

static const char *setting(int kind) { return kind==IGGLE_MOD_WORLD?"world":"hero"; }

void iggle_mods_startup(void)
{
    if (initialized) return;
    initialized=TRUE;
    char temp[MAX_PATH], unique[64];
    DWORD length=GetTempPathA(MAX_PATH,temp);
    if (!length || length>=MAX_PATH) return;
    snprintf(unique,sizeof unique,"IgglePopMods-%lu-%lu",GetCurrentProcessId(),GetTickCount());
    if (!path_join(cache,temp,unique) || !CreateDirectoryA(cache,NULL) ||
        !path_join(settings,g_config.game_path,"mods\\active.ini")) {
        cache[0]=0;
        return;
    }
    iggle_mods_scan();
    for (int i=0;i<count;++i) {
        Mod *mod=&mods[i];
        int kind=mod->info.kind;
        char selected[IGGLE_MOD_NAME], folder[MAX_PATH];
        GetPrivateProfileStringA("mods",setting(kind),"",selected,ARRAYSIZE(selected),settings);
        if (!strcmp(selected,mod->info.id) && mount(mod,folder)) {
            strcpy(active[kind],mod->info.id);
            strcpy(mounted[kind],folder);
        }
    }
}

BOOL iggle_mods_info(int index,IggleModInfo *info)
{
    AcquireSRWLockShared(&mods_lock);
    BOOL ok=index>=0 && index<count;
    if (ok) {
        *info=mods[index].info;
        info->enabled=!strcmp(active[info->kind],info->id);
    }
    ReleaseSRWLockShared(&mods_lock);
    return ok;
}

BOOL iggle_mods_toggle(int index)
{
    if (!cache[0]) return FALSE;
    AcquireSRWLockExclusive(&mods_lock);
    BOOL ok=index>=0 && index<count;
    if (ok) {
        const Mod *mod=&mods[index];
        int kind=mod->info.kind;
        BOOL enable=strcmp(active[kind],mod->info.id)!=0;
        char folder[MAX_PATH]="";
        ok=(!enable || mount(mod,folder)) &&
            WritePrivateProfileStringA("mods",setting(kind),enable?mod->info.id:NULL,settings);
        if (ok) {
            strcpy(active[kind],enable?mod->info.id:"");
            strcpy(mounted[kind],folder);
        }
    }
    ReleaseSRWLockExclusive(&mods_lock);
    return ok;
}

BOOL iggle_mods_world(char id[IGGLE_MOD_NAME],char name[IGGLE_MOD_NAME])
{
    AcquireSRWLockShared(&mods_lock);
    int i=0;
    while (i<count && (mods[i].info.kind!=IGGLE_MOD_WORLD || strcmp(mods[i].info.id,active[IGGLE_MOD_WORLD]))) ++i;
    BOOL ok=i<count;
    if (ok && id) strcpy(id,mods[i].info.id);
    if (ok && name) strcpy(name,mods[i].info.name);
    ReleaseSRWLockShared(&mods_lock);
    return ok;
}

static BOOL mounted_path(int kind,const char *name,char path[MAX_PATH])
{
    AcquireSRWLockShared(&mods_lock);
    BOOL ok=mounted[kind][0] && path_join(path,mounted[kind],name);
    ReleaseSRWLockShared(&mods_lock);
    if (ok) {
        DWORD attrs=GetFileAttributesA(path);
        ok=attrs!=INVALID_FILE_ATTRIBUTES && !(attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
    }
    return ok;
}
BOOL iggle_mods_resource(const char *resource,BOOL world,char path[MAX_PATH])
{
    char normalized[MAX_PATH];
    if (!resource || strlen(resource)>=sizeof normalized) return FALSE;
    strcpy(normalized,resource);
    for (char *c=normalized;*c;++c) if (*c=='\\') *c='/';
    const char *const world_prefix="res/igglemods/world/";
    const char *const hero_prefix="res/themes/fuzzles/";
    const char *name;
    if (world && !_strnicmp(normalized,world_prefix,strlen(world_prefix))) {
        const char *id=normalized+strlen(world_prefix);
        const char *slash=strchr(id,'/');
        char selected[IGGLE_MOD_NAME];
        if (!slash || slash-id>=IGGLE_MOD_NAME || !iggle_mods_world(selected,NULL) ||
            strlen(selected)!=(size_t)(slash-id) || _strnicmp(id,selected,slash-id)) return FALSE;
        name=slash+1;
        if (!_stricmp(name,"theme.xml")) return mounted_path(IGGLE_MOD_WORLD,"theme.xml",path);
        for (unsigned i=0;i<ARRAYSIZE(world_files);++i)
            if (!_stricmp(name,world_files[i])) return mounted_path(IGGLE_MOD_WORLD,world_files[i],path);
    }
    if (!_strnicmp(normalized,hero_prefix,strlen(hero_prefix))) {
        name=normalized+strlen(hero_prefix);
        for (unsigned i=0;i<ARRAYSIZE(hero_files);++i)
            if (!_stricmp(name,hero_files[i])) return mounted_path(IGGLE_MOD_HERO,hero_files[i],path);
    }
    return FALSE;
}
BOOL iggle_mods_stage(wchar_t path[MAX_PATH])
{
    char stage[MAX_PATH];
    return mounted_path(IGGLE_MOD_WORLD,"stage.xml",stage) &&
        MultiByteToWideChar(CP_ACP,0,stage,-1,path,MAX_PATH)>0;
}
BOOL iggle_mods_preview(int tile,char path[MAX_PATH])
{
    static const char *const names[]={"floor.rgba","filled.rgba","wallh.rgba","wallv.rgba"};
    return tile>=0 && tile<ARRAYSIZE(names) && mounted_path(IGGLE_MOD_WORLD,names[tile],path);
}
