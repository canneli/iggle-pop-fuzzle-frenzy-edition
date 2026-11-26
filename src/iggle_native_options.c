#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <math.h>
#include <shellapi.h>
#include "dd.h"
#include "config.h"
#include "mouse.h"
#include "iggle_input.h"
#include "iggle_menu.h"
#include "iggle_custom_levels.h"
#include "iggle_level_editor.h"
#include "iggle_mods.h"
#include "iggle_voice.h"

typedef void (__attribute__((thiscall)) *ActionFunction)(void *,void *);
typedef void (__attribute__((thiscall)) *PhaseFunction)(void *);
typedef void (__attribute__((thiscall)) *PhaseUpdate)(void *, void *);
static ActionFunction original_options, original_main, original_phase;
static ActionFunction original_retro, original_bonus, original_active_update;
static BYTE *game_image;
static BOOL in_pause_menu;
static void *custom_state, *custom_menu_state;
static BYTE *custom_game;
static BOOL custom_completed;
static char custom_path[MAX_PATH], custom_temporary[MAX_PATH];
static const char custom_level_resource[] = "res/igglemods/custom-stage.xml";
static BOOL custom_running, custom_ready, custom_exit_pending;
static BOOL loading_custom_objects;
static BOOL voice_actor_probe(void *actor,IggleVoiceActor *state);
static const char *custom_theme;
static DWORD saved_level_index, saved_level_number;
#ifdef IGGLE_CUSTOM_TEST
static void custom_trace(const char *message);
#endif
static void __attribute__((thiscall)) phase_hook(void *self) {
    ((PhaseFunction)original_phase)(self);
    in_pause_menu = FALSE;
    iggle_menu_clear();
    iggle_input_set_gameplay(TRUE);
}

static void __attribute__((thiscall)) retro_hook(void *self)
{
    ((PhaseFunction)original_retro)(self);
    in_pause_menu = FALSE;
    iggle_menu_clear();
    iggle_input_set_gameplay(TRUE);
}

static void __attribute__((thiscall)) bonus_hook(void *self)
{
    ((PhaseFunction)original_bonus)(self);
    in_pause_menu = FALSE;
    iggle_menu_clear();
    iggle_input_set_gameplay(TRUE);
}

static void __attribute__((thiscall)) active_update_hook(void *phase, void *frame)
{
    ((PhaseUpdate)original_active_update)(phase, frame);
    BYTE *world = phase;
    if (world[0x14d]) return; // The engine is still counting down.
    if(in_pause_menu || custom_exit_pending || iggle_overlay_is_open())iggle_voice_stop();
    if(!in_pause_menu && !custom_exit_pending && !iggle_overlay_is_open())
        iggle_voice_tick(*(float *)((BYTE *)frame+8),voice_actor_probe);

    void *bonus_screen = *(void **)(world + 0x12c);
    if (bonus_screen && *(void **)bonus_screen == game_image + 0x116e20) {
        typedef void (__attribute__((thiscall)) *SetVisible)(void *, BOOL);
        // Toggle the attached screen through its native visibility method.
        void **vtable = *(void ***)bonus_screen;
#ifdef IGGLE_CUSTOM_TEST
        BOOL was_visible = !(*(DWORD *)((BYTE *)bonus_screen + 0x74) & 1);
#endif
        ((SetVisible)vtable[0x78 / sizeof(void *)])(bonus_screen, FALSE);
#ifdef IGGLE_CUSTOM_TEST
        if (was_visible && (*(DWORD *)((BYTE *)bonus_screen + 0x74) & 1))
            custom_trace("bonus banner hidden after countdown");
#endif
    }
}

// This screen loader takes the game's 28-byte string by value.
typedef struct {
    DWORD allocator;
    union { char inline_text[16]; const char *heap_text; } value;
    DWORD length;
    DWORD capacity;
} GameString;

typedef BYTE (__attribute__((thiscall)) *LoadLayout)(void *, GameString);
static LoadLayout original_layout_loader;
static ActionFunction original_pause_action;
typedef void (__attribute__((thiscall)) *QueueScreen)(void *,void *,float);
static QueueScreen original_queue_screen;
static struct {void *screen; char name[48];} native_screens[256];
static unsigned native_screen_count;

static void track_menu_screen(void *screen,const char *path)
{
    const char *name=path;
    for (const char *p=path;*p;++p) if (*p=='/' || *p=='\\') name=p+1;
    if (strlen(name)>=sizeof native_screens[0].name) return;
    unsigned i=0;
    while (i<native_screen_count && native_screens[i].screen!=screen) ++i;
    if (i==ARRAYSIZE(native_screens)) return;
    if (i==native_screen_count) ++native_screen_count;
    native_screens[i].screen=screen;
    strcpy(native_screens[i].name,name);
}

static void reset_restored_objects(void);
static void reset_crystal_power(void);
static BOOL action_is(void *action, const char *label);

static BYTE __attribute__((thiscall)) layout_hook(void *screen, GameString file)
{
    const char *path = file.capacity < 16 ? file.value.inline_text : file.value.heap_text;
    if (path && file.length < MAX_PATH && file.length <= file.capacity) {
        char name[MAX_PATH];
        memcpy(name, path, file.length);
        name[file.length] = 0;
        track_menu_screen(screen,name);
#ifdef IGGLE_CUSTOM_TEST
        custom_trace(name);
#endif
        if (strstr(name, "pauseMenu.xml")) in_pause_menu = TRUE;
        if (strstr(name, "mainMenu.xml")) {
            in_pause_menu = FALSE;
            reset_crystal_power();
            reset_restored_objects();
            custom_exit_pending = FALSE;
            custom_game = NULL;
            custom_running = FALSE;
            custom_state = NULL;
        }
        if (in_pause_menu || strstr(name, "mainMenu.xml") || strstr(name, "gameEndRetro.xml") ||
            strstr(name, "levelEndNormal.xml") || strstr(name, "levelEndBonus.xml")) {
            iggle_voice_stop();
            iggle_input_set_gameplay(FALSE);
        }
    }
    return original_layout_loader(screen, file);
}

typedef GameString *(__attribute__((thiscall)) *MakeString)(GameString *, const char *);
typedef BYTE (__attribute__((thiscall)) *LoadLevel)(void *, GameString *);
typedef void *(__attribute__((thiscall)) *BuildState)(void *, void *, int);
typedef void *(__attribute__((thiscall)) *OpenStream)(void *, const char *);
typedef void (__attribute__((thiscall)) *QueueState)(void *, void *);

static PhaseFunction original_test_init, original_test_enter;
static ActionFunction original_test_action;
static OpenStream original_open_stream, original_raw_stream, original_archive_stream;
typedef BYTE (__attribute__((thiscall)) *ResourceExists)(void *,const char *);
static ResourceExists original_resource_exists;

#ifdef IGGLE_CUSTOM_TEST
static LONG CALLBACK trace_custom_exception(EXCEPTION_POINTERS *ex)
{
    FILE *f = fopen("custom-smoke.log", "a");
    if (!f) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(f, "Exception %08lx at %p\n", ex->ExceptionRecord->ExceptionCode, ex->ExceptionRecord->ExceptionAddress);
    DWORD *sp = (DWORD *)ex->ContextRecord->Esp;
    for (int i = 0; i < 40; ++i)
        if (sp[i] >= (DWORD)game_image + 0x1000 && sp[i] < (DWORD)game_image + 0x10d000)
            fprintf(f, " %x", (unsigned)sp[i]);
    fputc(10, f);
    fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;
}
static void custom_trace(const char *message)
{
    FILE *f = fopen("custom-smoke.log", "a");
    if (f) { fprintf(f, "%s\n", message); fclose(f); }
}
#else
#define custom_trace(message) ((void)0)
#endif

static void __attribute__((thiscall)) queue_screen_hook(void *manager,void *screen,float transition)
{
    original_queue_screen(manager,screen,transition);
    for (unsigned i=0;screen && i<native_screen_count;++i) {
        if (native_screens[i].screen!=screen) continue;
        const char *name=native_screens[i].name;
        if (iggle_menu_load(name)) iggle_input_set_gameplay(FALSE);
#ifdef IGGLE_CUSTOM_TEST
        char info[80];snprintf(info,sizeof info,"queued native screen %s",name);custom_trace(info);
#endif
        break;
    }
}

typedef void *(__attribute__((thiscall)) *SpawnObject)(void *, void *, void *, void *, void *, int);
typedef void *(__attribute__((thiscall)) *SpawnPickup)(void *, void *, int);
typedef void *(__attribute__((thiscall)) *FindArt)(void *, GameString *);
typedef GameString *(__attribute__((thiscall)) *ReadXmlAttribute)(void *, GameString *, GameString *, GameString *);
enum { ART_NORMAL, ART_SLOW, ART_RAINBOW, ART_DRAGON, ART_BLOBBY };
static SpawnObject original_slow_spawner, original_rainbow_spawner, original_dragon_spawner;
static SpawnPickup original_spawn_pickup;
static FindArt original_find_art;
static FindArt original_find_particle;
static void *blobby_eyes[3];
static void *blobby_images[4][8];
static struct {void *sprite,*actor;float x,y;unsigned direction;DWORD next_turn;BOOL positioned;} blobby_drawables[EDIT_GRID_MAX*EDIT_GRID_MAX];
static void *current_blobby_actor;
static unsigned blobby_drawable_count;
static BOOL creating_fireball;
static void *fireballs[1024];
static unsigned fireball_count;
static SpawnPickup original_projectile_constructor;
static ActionFunction original_projectile_art, original_projectile_other_hit;
static struct { void *actor; float cooldown, windup, animation; int direction; } dragon_combat[EDIT_GRID_MAX*EDIT_GRID_MAX];
static unsigned dragon_combat_count;
static void update_dragon_combat(void *actor, void *frame);
static ActionFunction original_pickup_collision, original_pickup_show, original_pickup_reset;
static ActionFunction original_dragon_activate, original_cage_show, original_dragon_create;
static ActionFunction original_world_step, original_attach_cage, original_monster_draw, original_dragon_init;
static ActionFunction original_spawn_monster_update;
static ActionFunction original_blobby_init, original_monster_tick, original_freed_fuzzle;
static SpawnPickup original_spawn_cage, original_spawn_monster;
static SpawnPickup original_make_sprite;
static int restored_art;
static void *slow_pickups[EDIT_GRID_MAX*EDIT_GRID_MAX], *slow_world;
static void *rainbow_cages[EDIT_GRID_MAX*EDIT_GRID_MAX];
static struct { void *object, *definition; int art; } restored_monsters[EDIT_GRID_MAX*EDIT_GRID_MAX];
static unsigned slow_count, rainbow_count, restored_monster_count;
static void *rainbow_fuzzles[EDIT_GRID_MAX*EDIT_GRID_MAX];
static unsigned rainbow_fuzzle_count;
typedef struct {int x,y; unsigned steps; BOOL positioned;} RainbowTrail;
static RainbowTrail rainbow_trails[ARRAYSIZE(rainbow_fuzzles)];
static SRWLOCK diamond_lock=SRWLOCK_INIT;
static IggleDiamondView diamonds;
static float diamond_drop_delay;
typedef void (__attribute__((thiscall)) *JewelScore)(void *,int,const float *);
static JewelScore original_collect_jewel;
static BOOL jewel_ready, crystal_shield_ready;
static void *shield_grace_player;
static float shield_grace_seconds;
static PhaseFunction original_fatal_hit, original_direct_fatal_hit;
static ActionFunction original_world_fatal_hit;
static void __attribute__((thiscall)) collect_native_jewel(void *world,int points,const float *position);
static PhaseFunction original_fuzzle_cleanup;
static PhaseFunction original_fuzzle_breakfree;
static void __attribute__((thiscall)) voiced_fuzzle_breakfree(void *fuzzle)
{
    original_fuzzle_breakfree(fuzzle);
    iggle_voice_lost(fuzzle);
}
static void remember_rainbow(void *fuzzle);
static BOOL actor_position(void *actor,float *x,float *y);
static void *dragon_images[4][8], *dragon_sprite_image;
static BOOL creating_dragon_sprite;
static struct { void *sprite, *actor; float x, y; DWORD next_turn; unsigned direction; BOOL positioned; } dragon_drawables[EDIT_GRID_MAX*EDIT_GRID_MAX];
static unsigned dragon_drawable_count;
static void *current_dragon_actor;
static ActionFunction original_fireball_player_hit;
static void *dragon_slow_player;
static float dragon_slow_seconds;
static void *current_player;
static ActionFunction original_player_step;
#ifdef IGGLE_CUSTOM_TEST
static BOOL logged_slow_frame;
#endif

void iggle_end_slow_power(void)
{
    KillTimer(g_ddraw.hwnd, IGGLE_SLOW_TIMER);
    if (!slow_world) return;
    slow_world = NULL;
    custom_trace("slow power expired");
}

static void reset_crystal_power(void)
{
    AcquireSRWLockExclusive(&diamond_lock);
    memset(&diamonds,0,sizeof diamonds);
    ReleaseSRWLockExclusive(&diamond_lock);
}

static void reset_restored_objects(void)
{
    iggle_voice_reset();
    iggle_end_slow_power();
    slow_count = 0;
    rainbow_count = restored_monster_count = 0;
    rainbow_fuzzle_count = 0;
    AcquireSRWLockExclusive(&diamond_lock);
    diamonds.count=0;
    diamonds.positioned=FALSE;
    diamonds.burst_seconds=0;
    diamonds.grace_seconds=0;
    shield_grace_player=NULL;
    shield_grace_seconds=0;
    diamond_drop_delay=0;
    ReleaseSRWLockExclusive(&diamond_lock);
    dragon_drawable_count = 0;
    blobby_drawable_count=0;
    current_blobby_actor=NULL;
    dragon_sprite_image = NULL;
    creating_dragon_sprite = FALSE;
    memset(dragon_images,0,sizeof dragon_images);
    memset(blobby_eyes,0,sizeof blobby_eyes);
    memset(blobby_images,0,sizeof blobby_images);
    fireball_count = dragon_combat_count = 0;
    creating_fireball = FALSE;
    current_dragon_actor = NULL;
    dragon_slow_player=NULL;
    dragon_slow_seconds=0;
    current_player=NULL;
    restored_art = ART_NORMAL;
}

static BOOL object_attribute_is(void *node, const GameString *attribute, const char *variant)
{
    if (!custom_running || !loading_custom_objects || !node) return FALSE;
    GameString key = *attribute;
    GameString empty = {.capacity=15}, value = {0};
    ((ReadXmlAttribute)(game_image+0x86910))(node, &value, &key, &empty);
    BOOL match = action_is(&value, variant);
    ((PhaseFunction)(game_image+0x4f890))(&value);
    return match;
}
static BOOL restored_object(void *node, const char *variant)
{
    static const GameString key = {.value.inline_text="hdRestored", .length=10, .capacity=15};
    return object_attribute_is(node, &key, variant);
}

static void *spawn_with_art(SpawnObject spawn, int art, const char *variant,
                            void *self, void *name, void *context, void *node, void *theme, int derived)
{
    int previous = restored_art;
    if (restored_object(node, variant)) restored_art = art;
    if (restored_art != previous) custom_trace(variant);
    void *result = spawn(self, name, context, node, theme, derived);
    restored_art = previous;
    return result;
}

static void *__attribute__((thiscall)) slow_spawner(void *self, void *name, void *context, void *node, void *theme, int derived)
{
    return spawn_with_art(original_slow_spawner, ART_SLOW, "Slow", self, name, context, node, theme, derived);
}

static void *__attribute__((thiscall)) rainbow_spawner(void *self, void *name, void *context, void *node, void *theme, int derived)
{
    return spawn_with_art(original_rainbow_spawner, ART_RAINBOW, "Rainbow", self, name, context, node, theme, derived);
}

static void *__attribute__((thiscall)) dragon_spawner(void *self, void *name, void *context, void *node, void *theme, int derived)
{
    static const GameString type = {.value.inline_text="type", .length=4, .capacity=15};
    int previous = restored_art;
    if (object_attribute_is(node, &type, "HD Dragon")) restored_art = ART_DRAGON;
    else if (object_attribute_is(node, &type, "Basic Blobby") || object_attribute_is(node, &type, "Med Blobby"))
        restored_art = ART_BLOBBY;
    unsigned first_monster = restored_monster_count;
    void *result = original_dragon_spawner(self, name, context, node, theme, derived);
    for (unsigned i=first_monster;i<restored_monster_count;++i)
        restored_monsters[i].definition = *(void **)((BYTE *)restored_monsters[i].object+0x4c);
    restored_art = previous;
    return result;
}

static int restored_index(void *const *objects, unsigned count, void *object)
{
    for (unsigned i=0; i<count; i++)
        if (objects[i] == object) return (int)i;
    return -1;
}

static void *__attribute__((thiscall)) spawn_cage(void *self, void *world, int derived)
{
    if (restored_art == ART_RAINBOW && rainbow_count < ARRAYSIZE(rainbow_cages))
        rainbow_cages[rainbow_count++] = self;
    return original_spawn_cage(self, world, derived);
}

static void __attribute__((thiscall)) show_cage(void *self, void *args)
{
    int previous = restored_art;
    if (restored_index(rainbow_cages, rainbow_count, self) >= 0) restored_art = ART_RAINBOW;
    if (restored_art == ART_RAINBOW) custom_trace("rainbow cage display");
    original_cage_show(self, args);
    restored_art = previous;
}

static void __attribute__((thiscall)) attach_cage(void *fuzzle, void *cage)
{
    int previous = restored_art;
    void *owned_cage = *(void **)((BYTE *)fuzzle+0xa4);
    if (restored_index(rainbow_cages, rainbow_count, cage) >= 0 ||
        restored_index(rainbow_cages, rainbow_count, owned_cage) >= 0) {
        restored_art = ART_RAINBOW;
        remember_rainbow(fuzzle);
    }
    original_attach_cage(fuzzle, cage);
    iggle_voice_register(fuzzle);
    restored_art = previous;
}

static ActionFunction original_trapped_art;
static void __attribute__((thiscall)) show_trapped_fuzzle(void *fuzzle, void *args)
{
    iggle_voice_register(fuzzle);
    void *previous_voice=iggle_voice_build(fuzzle);
    int previous = restored_art;
    void *cage = *(void **)((BYTE *)fuzzle+0xa4);
    if (restored_index(rainbow_cages, rainbow_count, cage) >= 0) {
        restored_art = ART_RAINBOW;
        remember_rainbow(fuzzle);
    }
    original_trapped_art(fuzzle, args);
    iggle_voice_build(previous_voice);
    restored_art = previous;
}

static void __attribute__((thiscall)) free_rainbow_fuzzle(void *fuzzle, void *args)
{
    void *previous_voice=iggle_voice_build(fuzzle);
    int previous = restored_art;
    if (restored_index(rainbow_fuzzles,rainbow_fuzzle_count,fuzzle)>=0) restored_art=ART_RAINBOW;
    original_freed_fuzzle(fuzzle,args);
    iggle_voice_build(previous_voice);
    restored_art=previous;
}

static void *__attribute__((thiscall)) spawn_monster(void *self, void *world, int derived)
{
    if ((restored_art == ART_DRAGON || restored_art == ART_BLOBBY) &&
        restored_monster_count < ARRAYSIZE(restored_monsters)) {
        restored_monsters[restored_monster_count].object = self;
        restored_monsters[restored_monster_count].definition = NULL;
        restored_monsters[restored_monster_count++].art = restored_art;
    }
    return original_spawn_monster(self, world, derived);
}

static void select_monster_art(void *object)
{
    restored_art=ART_NORMAL;
    if(!custom_running)return;
    for (unsigned i=0; i<restored_monster_count; ++i) {
        if (restored_monsters[i].object == object) {
            restored_art = restored_monsters[i].art;
            return;
        }
    }
}

static BOOL matches_spawn(void *object, int art)
{
    if (!custom_running || !object) return FALSE;
    void *definition = *(void **)((BYTE *)object+0x28);
    for (unsigned i=0;i<restored_monster_count;++i)
        if (restored_monsters[i].art == art && restored_monsters[i].definition == definition) return TRUE;
    return FALSE;
}

static void __attribute__((thiscall)) activate_dragon(void *self, void *args)
{
    int previous = restored_art;
    select_monster_art(self);
    if (restored_art == ART_DRAGON) custom_trace("dragon activation");
    original_dragon_activate(self, args);
    restored_art = previous;
}

static void __attribute__((thiscall)) create_dragon(void *self, void *args)
{
    int previous = restored_art;
    select_monster_art(self);
    original_dragon_create(self, args);
    restored_art = previous;
}

static void __attribute__((thiscall)) update_monster_spawner(void *self, void *frame)
{
    int previous = restored_art;
    select_monster_art(self);
    original_spawn_monster_update(self, frame);
    restored_art = previous;
}

static void __attribute__((thiscall)) init_dragon_art(void *self, void *args)
{
    BOOL previous = creating_dragon_sprite;
    int previous_art = restored_art;
    creating_dragon_sprite = restored_art == ART_DRAGON || matches_spawn(*(void **)((BYTE *)self+0x4c), ART_DRAGON);
    if (creating_dragon_sprite) restored_art = ART_DRAGON;
    void *previous_actor = current_dragon_actor;
    if (creating_dragon_sprite) current_dragon_actor=self;
    if (custom_running) custom_trace(creating_dragon_sprite?"dragon body init":"fire body init");
    original_dragon_init(self, args);
    if (creating_dragon_sprite && dragon_combat_count<ARRAYSIZE(dragon_combat)) {
        unsigned i=0;
        while(i<dragon_combat_count && dragon_combat[i].actor!=self) ++i;
        if(i==dragon_combat_count) {
            ++dragon_combat_count;
            dragon_combat[i].actor=self;
            dragon_combat[i].cooldown=1.4f;
            dragon_combat[i].windup=0;
            dragon_combat[i].animation=1.f;
        }
    }
    current_dragon_actor=previous_actor;
    restored_art = previous_art;
    creating_dragon_sprite = previous;
}

static void __attribute__((thiscall)) init_blobby_art(void *self, void *args)
{
    int previous = restored_art;
    void *previous_actor=current_blobby_actor;
    if (matches_spawn(*(void **)((BYTE *)self+0x4c), ART_BLOBBY)) restored_art = ART_BLOBBY;
    if (restored_art == ART_BLOBBY) custom_trace("blobby body init");
    if (restored_art == ART_BLOBBY) current_blobby_actor=self;
    original_blobby_init(self, args);
    current_blobby_actor=previous_actor;
    restored_art = previous;
}

static void __attribute__((thiscall)) tick_monster_art(void *self, void *frame)
{
    if (!custom_running && diamonds.slow_seconds<=0) {
        original_monster_tick(self,frame);
        return;
    }
    int previous = restored_art;
    void *body = *(void **)((BYTE *)self+0x4c);
    if (matches_spawn(body,ART_DRAGON)) restored_art=ART_DRAGON;
    else if (matches_spawn(body,ART_BLOBBY)) restored_art=ART_BLOBBY;
    float *delta=(float *)((BYTE *)frame+8), elapsed=*delta;
    if (diamonds.slow_seconds>0) *delta=elapsed*.26f;
    original_monster_tick(self,frame);
    if (restored_art==ART_DRAGON) update_dragon_combat(self,frame);
    *delta=elapsed;
    restored_art=previous;
}

static void *__attribute__((thiscall)) make_monster_sprite(void *sprite, void *resource, int derived)
{
    void *result = original_make_sprite(sprite, resource, derived);
    iggle_voice_sprite(result,resource);
    if(current_blobby_actor && resource && (resource==blobby_eyes[0] || resource==blobby_eyes[1] || resource==blobby_eyes[2])) {
        unsigned index=0;
        while(index<blobby_drawable_count && blobby_drawables[index].sprite!=result) ++index;
        if(index<ARRAYSIZE(blobby_drawables)) {
            if(index==blobby_drawable_count) ++blobby_drawable_count;
            blobby_drawables[index].sprite=result;blobby_drawables[index].actor=current_blobby_actor;
            blobby_drawables[index].x=*(int *)((BYTE*)current_blobby_actor+0x10);
            blobby_drawables[index].y=*(int *)((BYTE*)current_blobby_actor+0x14);
            blobby_drawables[index].direction=0;blobby_drawables[index].next_turn=0;
            blobby_drawables[index].positioned=FALSE;
        }
    }
    if (creating_dragon_sprite && result && resource && resource == dragon_sprite_image) {
        unsigned index = 0;
        while (index<dragon_drawable_count && dragon_drawables[index].sprite != result) ++index;
        if (index == ARRAYSIZE(dragon_drawables)) return result;
        if (index == dragon_drawable_count) ++dragon_drawable_count;
        dragon_drawables[index].sprite = result;
        dragon_drawables[index].actor = current_dragon_actor;
        dragon_drawables[index].x = 0;
        dragon_drawables[index].y = 0;
        dragon_drawables[index].direction = 0;
        dragon_drawables[index].positioned=FALSE;
        dragon_drawables[index].next_turn = GetTickCount();
        custom_trace("dragon drawable ready");
    }
    return result;
}

static void monster_visual_position(void *actor, void *sprite, float *x, float *y)
{
    BYTE *body=actor?*(BYTE **)((BYTE *)actor+0x4c):NULL;
    BYTE *root=body?*(BYTE **)(body+0xc):NULL;
    float *position=(float *)((BYTE *)(root?root:sprite)+0x2c);
    *x=position[0];*y=position[1];
}

static void __attribute__((thiscall)) draw_restored_monster(void *sprite, void *context)
{
    iggle_voice_draw(sprite);
    if(!custom_running) {original_monster_draw(sprite,context);return;}
    void **resource = (void **)((BYTE *)sprite+0x11c);
    void *original = *resource;
    BOOL blobby_eye = original && (original==blobby_eyes[0] || original==blobby_eyes[1] || original==blobby_eyes[2]);
    float *position = (float *)((BYTE *)sprite+0x2c);
    float eye_x = position[0], eye_y = position[1];
    typedef void (__attribute__((thiscall)) *SetPosition)(void *,float,float);
    SetPosition set_position = (SetPosition)(game_image+0x81a30);
    if (blobby_eye) {
        set_position(sprite,eye_x,eye_y+13.f);
        unsigned index=0;
        while(index<blobby_drawable_count && blobby_drawables[index].sprite!=sprite) ++index;
        if(index==blobby_drawable_count && index<ARRAYSIZE(blobby_drawables)) {
            ++blobby_drawable_count;blobby_drawables[index].sprite=sprite;
            blobby_drawables[index].actor=NULL;
            blobby_drawables[index].x=eye_x;blobby_drawables[index].y=eye_y;
            blobby_drawables[index].direction=0;blobby_drawables[index].next_turn=0;
            blobby_drawables[index].positioned=FALSE;
        }
        unsigned direction=0;
        if(index<blobby_drawable_count) {
            DWORD now=GetTickCount();
            if(!in_pause_menu && (LONG)(now-blobby_drawables[index].next_turn)>=0) {
                BYTE *actor=blobby_drawables[index].actor;
                float x,y;monster_visual_position(actor,sprite,&x,&y);
                float dx=x-blobby_drawables[index].x,dy=y-blobby_drawables[index].y;
                if(blobby_drawables[index].positioned && dx*dx+dy*dy>.0625f) blobby_drawables[index].direction=dx*dx>dy*dy?(dx>0?1:2):(dy>0?0:3);
                blobby_drawables[index].positioned=TRUE;
                blobby_drawables[index].x=x;blobby_drawables[index].y=y;blobby_drawables[index].next_turn=now+65;
            }
            direction=blobby_drawables[index].direction;
        }
        unsigned phase=(GetTickCount()+((uintptr_t)sprite%1100))%3600;
        unsigned frame=phase<90?1:phase<180?2:phase<270?3:4+(phase/125)%4;
        if(blobby_images[direction][frame]) *resource=blobby_images[direction][frame];
#ifdef IGGLE_CUSTOM_TEST
        static unsigned directions_traced;
        if(!(directions_traced&(1u<<direction))) {char message[50];snprintf(message,sizeof message,"blobby direction drawn %u",direction);custom_trace(message);directions_traced|=1u<<direction;}
        static BOOL blink_traced;
        if(frame==2 && !blink_traced && *resource!=original) {custom_trace("blobby blink drawn");blink_traced=TRUE;}
#endif
    }
    if (original && original == dragon_sprite_image) {
        for (unsigned i=0; i<dragon_drawable_count; ++i) {
            if (dragon_drawables[i].sprite != sprite) continue;
            DWORD now = GetTickCount();
            if ((LONG)(now-dragon_drawables[i].next_turn) >= 0 && !in_pause_menu) {
                float x,y;monster_visual_position(dragon_drawables[i].actor,sprite,&x,&y);
                float dx = x-dragon_drawables[i].x, dy = y-dragon_drawables[i].y;
                if (dragon_drawables[i].positioned && dx*dx+dy*dy > 0.0625f)
                    dragon_drawables[i].direction = dx*dx>dy*dy ? (dx>0?1:2) : (dy>0?0:3);
                dragon_drawables[i].positioned=TRUE;
                dragon_drawables[i].x = x;
                dragon_drawables[i].y = y;
                dragon_drawables[i].next_turn = now+65;
            }
            unsigned frame = 0;
            unsigned eye_phase=(now+((uintptr_t)sprite%1300))%4500;
            frame=eye_phase<100?6:eye_phase<190?7:eye_phase<290?6:eye_phase<1800?4:eye_phase<3300?5:0;
            for(unsigned j=0;j<dragon_combat_count;++j) {
                if(dragon_combat[j].actor!=dragon_drawables[i].actor) continue;
                float animation=dragon_combat[j].animation;
                if(animation<.62f) frame=animation<.28f?1:animation<.46f?2:3;
                if(animation<.62f) {
                    const int *vector=(const int *)(game_image+0x148e10)+dragon_combat[j].direction*2;
                    dragon_drawables[i].direction=vector[0]? (vector[0]>0?1:2) : (vector[1]>0?0:3);
                }
                break;
            }
            void *selected = dragon_images[dragon_drawables[i].direction][frame];
            if (selected) {
                *resource = selected;
#ifdef IGGLE_CUSTOM_TEST
                static BOOL attack_traced;
                if (frame == 2 && !attack_traced) {
                    attack_traced = TRUE;
                    custom_trace("dragon fire frame drawn");
                }
                static BOOL dragon_blink_traced;
                if(frame==7 && dragon_drawables[i].direction!=3 && !dragon_blink_traced) {custom_trace("dragon blink drawn");dragon_blink_traced=TRUE;}
#endif
            }
            break;
        }
    }
    original_monster_draw(sprite, context);
    *resource = original;
    if (blobby_eye) set_position(sprite,eye_x,eye_y);
}

static void *__attribute__((thiscall)) spawn_pickup(void *self, void *world, int derived)
{
    if (restored_art == ART_SLOW && slow_count < sizeof slow_pickups / sizeof *slow_pickups)
        slow_pickups[slow_count++] = self;
    return original_spawn_pickup(self, world, derived);
}

static void show_pickup(ActionFunction show, void *self, void *arg)
{
    int previous = restored_art;
    if (restored_index(slow_pickups, slow_count, self) >= 0) restored_art = ART_SLOW;
    show(self, arg);
    restored_art = previous;
}

static void __attribute__((thiscall)) pickup_show(void *self, void *arg)
{
    show_pickup(original_pickup_show, self, arg);
}

static void __attribute__((thiscall)) pickup_reset(void *self, void *arg)
{
    show_pickup(original_pickup_reset, self, arg);
}

static void __attribute__((thiscall)) pickup_collision(void *self, void *collider)
{
    int index = restored_index(slow_pickups, slow_count, self);
    if (index < 0) { original_pickup_collision(self, collider); return; }
    BYTE *pickup = self;
    if (pickup[0x44]) return;
    void *world = *(void **)(pickup+0x40);
    if (SetTimer(g_ddraw.hwnd, IGGLE_SLOW_TIMER, 6000, NULL)) {
        slow_world = world;
#ifdef IGGLE_CUSTOM_TEST
        logged_slow_frame = FALSE;
#endif
        custom_trace("slow pickup collected");
    }
    pickup[0x44] = 1;
    ((PhaseFunction)(game_image+0x8eb80))(*(void **)(pickup+0x4c));
}

static void __attribute__((thiscall)) step_restored_time(void *phase, void *frame)
{
    if (!slow_world || *(void **)((BYTE *)phase+0x0c) != slow_world) {
        original_world_step(phase, frame);
        return;
    }
    float *delta = (float *)((BYTE *)frame+8);
    float elapsed = *delta;
    *delta = elapsed*0.5f;
#ifdef IGGLE_CUSTOM_TEST
    if (!logged_slow_frame) { custom_trace("slow frame scaled"); logged_slow_frame = TRUE; }
#endif
    original_world_step(phase, frame);
    *delta = elapsed;
}

static void *find_voiced_art(void *theme,GameString *name)
{
    void *image=original_find_art(theme,name);
    if(name && name->length<128 && name->length<=name->capacity) {
        char label[128];
        const char *text=name->capacity<16?name->value.inline_text:name->value.heap_text;
        if(text){memcpy(label,text,name->length);label[name->length]=0;iggle_voice_art(image,label);}
    }
    return image;
}

static void *__attribute__((thiscall)) find_restored_art(void *theme, GameString *name)
{
    // Continue may reach the first story dialog before the native hero theme
    // is initialized. Load the selected hero's original theme, not a substitute.
    if(!theme && (action_is(name,"docStoryPortrait") || action_is(name,"heroStoryPortrait") || action_is(name,"heroStoryPortraitLg"))) {
        static BOOL loading_story_theme;
        BYTE *app=*(BYTE **)(game_image+0x148c60);
        const BYTE expected[]={0x6a,0xff,0x68,0xb3,0xd1,0x4f,0x00};
        if(app && !loading_story_theme && !memcmp(game_image+0x3150,expected,sizeof expected)) {
            GameString *hero=(GameString *)(app+0x13c);
            GameString fallback={.value.inline_text="Fizzy",.length=5,.capacity=15};
            if(!action_is(hero,"Fizzy") && !action_is(hero,"Dizzy") && !action_is(hero,"Rizzo") && !action_is(hero,"Bozz") && !action_is(hero,"Doc"))hero=&fallback;
            loading_story_theme=TRUE;
            ((ActionFunction)(game_image+0x3150))(app,hero);
            loading_story_theme=FALSE;
            theme=*(void **)(app+0x158);
            custom_trace(theme?"native story hero theme initialized":"native story hero theme unavailable");
        }
    }

#ifdef IGGLE_CUSTOM_TEST
    if(action_is(name,"docStoryPortrait") || action_is(name,"heroStoryPortrait")) {
        custom_trace(action_is(name,"docStoryPortrait")?"story doc portrait requested":"story hero portrait requested");
        {char message[80];snprintf(message,sizeof message,"story portrait theme %p",theme);custom_trace(message);}
    }
#endif
    if(!custom_running)return find_voiced_art(theme,name);
    const char *alias = NULL;
    if (creating_fireball && action_is(name,"bubbleProjectile")) alias="hdDragonFireball";
    if (restored_art == ART_SLOW && action_is(name, "freezePickupBase")) alias = "slowPickupBase";
    const char *name_text = name->capacity < 16 ? name->value.inline_text : name->value.heap_text;
    if (restored_art == ART_RAINBOW && name->length >= 10 &&
        !_strnicmp(name_text, "trapped", 7)) {
        char frame = name_text[name->length-1];
        if (frame == '0') alias = "trappedRainbow0";
        if (frame == '1') alias = "trappedRainbow1";
    }
    char rainbow_label[32];
    if (restored_art == ART_RAINBOW && name->length>=8 && !_strnicmp(name_text,"follow",6)) {
        snprintf(rainbow_label,sizeof rainbow_label,"hdRainbow%c",name_text[name->length-1]);
        alias=rainbow_label;
    }
    if (restored_art == ART_DRAGON) {
        if (action_is(name, "hdDragon") || action_is(name, "fireZoogEditor")) alias = "hdDragon";
        else if (!_strnicmp(name_text, "fireHorns", 9)) alias = "hdMonsterBlank";
        else if (!_strnicmp(name_text,"genericZoogEyes",15)) alias = "hdDragon";
        else if (!_strnicmp(name_text,"fireZoogBlob",12) && name->length == 13) alias = "hdMonsterBlank";
    }
    if (restored_art == ART_BLOBBY) {
        if (action_is(name,"baseZoogEditor")) alias = "hdBlobby";
        else if (action_is(name,"genericZoogEyesSouth")) alias = "hdBlobbyEyesS";
        else if (action_is(name,"genericZoogEyesEast")) alias = "hdBlobbyEyesE";
        else if (action_is(name,"genericZoogEyesWest")) alias = "hdBlobbyEyesW";
        else if (action_is(name,"genericZoogBlob0")) alias = "hdMonsterBlank";
        else if (!_strnicmp(name_text,"genericZoogBlob",15) && name->length == 16) alias = "hdMonsterBlank";
    }
    if (!alias) return find_voiced_art(theme, name);
    GameString replacement = {0};
    ((MakeString)(game_image+0x3d30))(&replacement, alias);
    void *image = find_voiced_art(theme, &replacement);
    ((PhaseFunction)(game_image+0x4f890))(&replacement);
    if (image && restored_art == ART_BLOBBY) {
        if (!strcmp(alias,"hdBlobbyEyesS")) blobby_eyes[0]=image;
        if (!strcmp(alias,"hdBlobbyEyesE")) blobby_eyes[1]=image;
        if (!strcmp(alias,"hdBlobbyEyesW")) blobby_eyes[2]=image;
        if(!blobby_images[0][0]) for(unsigned direction=0;direction<4;++direction) {
            for(unsigned frame=0;frame<8;++frame) {
                char label[20]; snprintf(label,sizeof label,"blobby%c%u","sewn"[direction],frame);
                ((MakeString)(game_image+0x3d30))(&replacement,label);
                blobby_images[direction][frame]=original_find_art(theme,&replacement);
                ((PhaseFunction)(game_image+0x4f890))(&replacement);
            }
        }
    }
    if (image && creating_dragon_sprite && !_strnicmp(name_text,"genericZoogEyes",15) && !dragon_sprite_image) {
        dragon_sprite_image = image;
        for (unsigned direction=0;direction<4;++direction) {
            for (unsigned frame=0;frame<8;++frame) {
                char label[] = "dragon00";
                label[6] += direction;
                label[7] += frame;
                ((MakeString)(game_image+0x3d30))(&replacement,label);
                dragon_images[direction][frame] = original_find_art(theme,&replacement);
                ((PhaseFunction)(game_image+0x4f890))(&replacement);
            }
        }
    }
    if (image) custom_trace(alias);
    else custom_trace("replacement art missing");
    return image ? image : find_voiced_art(theme, name);
}

static void __attribute__((thiscall)) forget_rainbow_fuzzle(void *fuzzle)
{
    iggle_voice_forget(fuzzle);
    for (unsigned i = 0; i < rainbow_fuzzle_count; ++i) {
        if (rainbow_fuzzles[i] != fuzzle) continue;
        unsigned last = --rainbow_fuzzle_count;
        rainbow_fuzzles[i] = rainbow_fuzzles[last];
        rainbow_trails[i] = rainbow_trails[last];
        break;
    }
    original_fuzzle_cleanup(fuzzle);
}

static void remember_rainbow(void *fuzzle)
{
    if (rainbow_fuzzle_count >= ARRAYSIZE(rainbow_fuzzles) ||
        restored_index(rainbow_fuzzles,rainbow_fuzzle_count,fuzzle) >= 0) return;
    rainbow_fuzzles[rainbow_fuzzle_count] = fuzzle;
    rainbow_trails[rainbow_fuzzle_count++] = (RainbowTrail){0};
}

static BOOL actor_position(void *actor, float *x, float *y)
{
    void *tracker = *(void **)((BYTE *)actor+0x1c);
    if (!tracker) {
#ifdef IGGLE_CUSTOM_TEST
        static BOOL logged_tracker;
        if (!logged_tracker) { logged_tracker=TRUE;custom_trace("diamond actor tracker absent"); }
#endif
        return FALSE;
    }
    typedef float *(__attribute__((thiscall)) *Position)(void *);
    float *point = ((Position)(*(void ***)tracker)[2])(tracker);
    if (!point || !isfinite(point[0]) || !isfinite(point[1])) {
#ifdef IGGLE_CUSTOM_TEST
        static BOOL logged_point;
        if (!logged_point) { logged_point=TRUE;custom_trace("diamond actor position absent"); }
#endif
        return FALSE;
    }
    *x=point[0];
    *y=point[1];
    return TRUE;
}

static BOOL voice_actor_probe(void *actor,IggleVoiceActor *state)
{
    BYTE *fuzzle=actor;
    state->alive=!fuzzle[0x44];
    state->trapped=fuzzle[0x95]!=0;
    state->following=*(void **)(fuzzle+0x58)!=NULL;
    return actor_position(actor,&state->x,&state->y);
}

static void step_rainbow_diamonds(void *hero, float dt)
{
    float x,y;
    if (!actor_position(hero,&x,&y)) return;
    AcquireSRWLockExclusive(&diamond_lock);
    diamonds.hero_x=x;
    diamonds.hero_y=y;
    diamonds.positioned=TRUE;
    diamonds.orbit=fmodf(diamonds.orbit+dt*2.3f,6.2831853f);
    diamonds.burst_seconds=max(0.f,diamonds.burst_seconds-dt);
    if (diamonds.slow_seconds>0) {
        diamonds.slow_seconds=max(0.f,diamonds.slow_seconds-dt);
        if (diamonds.slow_seconds==0) custom_trace("crystal slowdown expired");
    }
    diamond_drop_delay=max(0.f,diamond_drop_delay-dt);
#ifdef IGGLE_CUSTOM_TEST
    if (custom_running && GetEnvironmentVariableA("IGGLE_DIAMOND_SMOKE",NULL,0)) {
        static BOOL seeded;
        if (!seeded) {
            seeded=TRUE;
            for (unsigned i=0;i<3;++i) diamonds.gems[diamonds.count++]=(IggleDiamond){x,y,0};
        }
    }
#endif
    for (unsigned i=0;i<diamonds.count;) {
        IggleDiamond *gem=&diamonds.gems[i];
        gem->age+=dt;
        float dx=x-gem->x,dy=y-gem->y;
        BOOL picked=gem->age>=.55f && dx*dx+dy*dy<=19.f*19.f;
        if (!picked && gem->age<15.f) { ++i; continue; }
        float position[]={gem->x,gem->y};
        *gem=diamonds.gems[--diamonds.count];
        if (picked && custom_running && custom_game) {
            void *world=*(void **)(custom_game+0xd8);
            ReleaseSRWLockExclusive(&diamond_lock);
            collect_native_jewel(world,100,position);
            AcquireSRWLockExclusive(&diamond_lock);
        }
    }
    if (jewel_ready && custom_running && custom_game && diamonds.count<IGGLE_DIAMOND_LIMIT && diamond_drop_delay<=0) {
        for (unsigned i=0;i<rainbow_fuzzle_count;++i) {
            BYTE *fuzzle=rainbow_fuzzles[i];
            RainbowTrail *trail=&rainbow_trails[i];
            if (fuzzle[0x44] || fuzzle[0x95] || !*(void **)(fuzzle+0x58)) {
                trail->positioned=FALSE;
                trail->steps=0;
                continue;
            }
            void *world=*(void **)(custom_game+0xd8);
            typedef BYTE *(__attribute__((thiscall)) *GetGrid)(void *);
            BYTE *grid=((GetGrid)(*(void ***)world)[0x1c/4])(world);
            if (!grid) continue;
            int size=*(int *)(grid+0x14),ox=*(int *)(grid+0x18),oy=*(int *)(grid+0x1c);
            float follower_x,follower_y;
            if (size<=0 || !actor_position(fuzzle,&follower_x,&follower_y)) continue;
            int cx=(int)floorf((follower_x-ox)/size),cy=(int)floorf((follower_y-oy)/size);
            if (!trail->positioned) {
                trail->x=cx; trail->y=cy; trail->positioned=TRUE;
                custom_trace("rainbow following player");
                continue;
            }
            if (trail->x==cx && trail->y==cy) continue;
            int from_x=trail->x,from_y=trail->y;
            trail->x=cx; trail->y=cy;
            ++trail->steps;
#ifdef IGGLE_CUSTOM_TEST
            BOOL force_drop=GetEnvironmentVariableA("IGGLE_DROP_SMOKE",NULL,0)!=0;
#else
            const BOOL force_drop=FALSE;
#endif
            if (!force_drop && (trail->steps<12 || rand()%30!=0)) continue;
            // Drop in the center of the tile just left, away from walls and the moving chain.
            if (from_x<0 || from_y<0 || from_x>=*(int *)(grid+0x10) || from_y>=*(int *)(grid+0xc))
                continue;
            float gx=ox+(from_x+.5f)*size,gy=oy+(from_y+.5f)*size;
            float dx=x-gx,dy=y-gy;
            if (dx*dx+dy*dy < 32.f*32.f) continue;
            diamonds.gems[diamonds.count++]=(IggleDiamond){gx,gy,0};
            custom_trace("rainbow diamond dropped");
            trail->steps=0;
            diamond_drop_delay=8.f;
            break;
        }
    }
    ReleaseSRWLockExclusive(&diamond_lock);
}

static void play_crystal_sound(void *theme, const char *name)
{
    if (!theme) return;
    GameString sound={0};
    ((MakeString)(game_image+0x3d30))(&sound,name);
    typedef void (__attribute__((thiscall)) *PlayThemeSound)(void *,GameString,float,float);
    ((PlayThemeSound)(game_image+0x246f0))(theme,sound,1.f,1.f);
    custom_trace(name);
}

static void __attribute__((thiscall)) collect_native_jewel(void *world,int points,const float *position)
{
    original_collect_jewel(world,points,position);
    if (!crystal_shield_ready) return;
    AcquireSRWLockExclusive(&diamond_lock);
    BOOL activated=++diamonds.collected==3;
    if (activated) {
        diamonds.collected=0;
        diamonds.shield=TRUE;
        diamonds.slow_seconds=5.f;
        diamonds.orbit=0;
        custom_trace("crystal shield activated");
    }
    custom_trace("native jewel collected");
    ReleaseSRWLockExclusive(&diamond_lock);
    if (activated) play_crystal_sound(*(void **)((BYTE *)world+0x20),"crystalshieldon");
}

static BOOL crystal_grace_active(void *player)
{
    return player && player==shield_grace_player && shield_grace_seconds>0;
}

static BOOL absorb_fatal_hit(BYTE *player)
{
    if (!player || *(BYTE **)player!=game_image+0x112358) return FALSE;
    if (crystal_grace_active(player)) {
        custom_trace("crystal grace blocked another hit");
        return TRUE;
    }
    if (!diamonds.shield) return FALSE;

    // Read the timer used by the original post-respawn state. Stay in the
    // active movement state so the hero keeps its input and maze collisions.
    shield_grace_player=player;
    shield_grace_seconds=*(const float *)(game_image+0x10f2e8);
    AcquireSRWLockExclusive(&diamond_lock);
    diamonds.shield=FALSE;
    diamonds.burst_seconds=.42f;
    diamonds.grace_seconds=shield_grace_seconds;
    ReleaseSRWLockExclusive(&diamond_lock);
    dragon_slow_player=NULL;
    dragon_slow_seconds=0;
    void *world=*(void **)(player+0xd4);
    if (world) play_crystal_sound(*(void **)((BYTE *)world+0x20),"crystalshieldbreak");
    custom_trace("crystal shield consumed; native grace started");
    return TRUE;
}

static void __attribute__((thiscall)) fatal_hit(void *state)
{
    if (!absorb_fatal_hit(*(BYTE **)((BYTE *)state+0xc))) {
        custom_trace("crystal fatal hit forwarded");
        original_fatal_hit(state);
    }
}

static void __attribute__((thiscall)) direct_fatal_hit(void *player)
{
    if (!absorb_fatal_hit(player)) original_direct_fatal_hit(player);
}

static void __attribute__((thiscall)) world_fatal_hit(void *world, void *source)
{
    // The world handler also disconnects the follower chain and changes the
    // level state. Intercept the hit before those side effects can run.
    BYTE *player=*(BYTE **)((BYTE *)world+0x118);
    if (source && absorb_fatal_hit(player)) {
        custom_trace("crystal blocked world hit before follower release");
        return;
    }
    original_world_fatal_hit(world,source);
}

BOOL iggle_diamond_view(IggleDiamondView *view)
{
    if (!TryAcquireSRWLockShared(&diamond_lock)) return FALSE;
    *view=diamonds;
    ReleaseSRWLockShared(&diamond_lock);
    return view->positioned &&
        (view->count || view->collected || view->shield || view->burst_seconds>0 || view->grace_seconds>0);
}

static void *__attribute__((thiscall)) construct_fireball(void *self, void *world, int derived)
{
    int old=restored_index(fireballs,fireball_count,self);
    if(old>=0) fireballs[old]=fireballs[--fireball_count];
    void *projectile=original_projectile_constructor(self,world,derived);
    if(creating_fireball && projectile && fireball_count<ARRAYSIZE(fireballs))
        fireballs[fireball_count++]=projectile;
    return projectile;
}

static void __attribute__((thiscall)) draw_fireball_art(void *self, void *args)
{
    BOOL previous=creating_fireball;
    if(restored_index(fireballs,fireball_count,self)>=0) creating_fireball=TRUE;
    original_projectile_art(self,args);
    creating_fireball=previous;
}

static void __attribute__((thiscall)) fireball_other_hit(void *state, void *target)
{
    void *projectile=*(void **)((BYTE *)state+0x10);
    if(restored_index(fireballs,fireball_count,projectile)>=0) {
        custom_trace("fireball ignored non-player");
        return;
    }
    original_projectile_other_hit(state,target);
}

static void __attribute__((thiscall)) fireball_player_hit(void *state, void *target)
{
    BYTE *projectile=*(BYTE **)((BYTE *)state+0x10);
    if (!crystal_grace_active(target)) {
        if (restored_index(fireballs,fireball_count,projectile)<0) {
            original_fireball_player_hit(state,target);
            return;
        }
        dragon_slow_player=target;
        dragon_slow_seconds=4.f;
        custom_trace("dragon fire slowed player without bubble");
    }
    void *sprite=*(void **)(projectile+0x48);
    if (sprite) ((ActionFunction)(*(void ***)sprite)[0x78/4])(sprite,NULL);
    projectile[0x44]=1;
}

static void __attribute__((thiscall)) step_dragon_slow_player(void *player, void *frame)
{
    if(*(BYTE **)player==game_image+0x112358) current_player=player;
    float frame_seconds=*(float *)((BYTE *)frame+8);
    if (crystal_grace_active(player) && !in_pause_menu && !custom_exit_pending &&
        frame_seconds>0.f && frame_seconds<.5f) {
        shield_grace_seconds=max(0.f,shield_grace_seconds-frame_seconds);
        AcquireSRWLockExclusive(&diamond_lock);
        diamonds.grace_seconds=shield_grace_seconds;
        ReleaseSRWLockExclusive(&diamond_lock);
#ifdef IGGLE_CUSTOM_TEST
        if (GetEnvironmentVariableA("IGGLE_GRACE_MOVEMENT_SMOKE",NULL,0)) {
            static DWORD next_sample;
            DWORD now=GetTickCount();
            float x,y;
            if ((LONG)(now-next_sample)>=0 && actor_position(player,&x,&y)) {
                char sample[110];
                BYTE *controller=(BYTE *)player+0xe0;
                typedef BYTE *(__attribute__((thiscall)) *GetState)(void *);
                BYTE *state=((GetState)(*(void ***)controller)[0x18/4])(controller);
                snprintf(sample,sizeof sample,"crystal grace pose %.2f %.2f %.2f %lx %d",
                    shield_grace_seconds,x,y,state?(unsigned long)(*(BYTE **)state-game_image):0,
                    ((void *(__attribute__((thiscall)) *)(void *))(game_image+0x66020))(player)!=NULL);
                custom_trace(sample);
                next_sample=now+80;
            }
        }
#endif
        if (shield_grace_seconds==0) {
            shield_grace_player=NULL;
            custom_trace("native crystal grace expired");
        }
    }
    if (player==current_player && !in_pause_menu && !custom_exit_pending &&
        frame_seconds>0.f && frame_seconds<.5f && (rainbow_fuzzle_count || diamonds.count || diamonds.slow_seconds>0 ||
        diamonds.shield || diamonds.collected || diamonds.burst_seconds>0 || diamonds.grace_seconds>0))
        step_rainbow_diamonds(player,frame_seconds);
    if(player!=dragon_slow_player || dragon_slow_seconds<=0) {original_player_step(player,frame);return;}
    float *delta=(float *)((BYTE *)frame+8), elapsed=*delta;
    if(!(elapsed>0 && elapsed<.5f)) {original_player_step(player,frame);return;}
    dragon_slow_seconds-=elapsed;
    *delta=elapsed*.45f;
    original_player_step(player,frame);
    *delta=elapsed;
    custom_trace("dragon player movement slowed");
    if(dragon_slow_seconds<=0) {dragon_slow_player=NULL;custom_trace("dragon player speed restored");}
}

static void update_dragon_combat(void *actor, void *frame)
{
    if(!custom_game || !custom_running || in_pause_menu || custom_exit_pending) return;
    BYTE *entity=actor;
    void *world=*(void **)(custom_game+0xd8);
    BYTE *hero=current_player;
    if(!hero) return;
    float delta=*(float *)((BYTE *)frame+8);
    if(!(delta>0.f && delta<.5f)) return;
    for(unsigned i=0;i<dragon_combat_count;++i) {
        if(dragon_combat[i].actor!=actor) continue;
        dragon_combat[i].animation+=delta;
        dragon_combat[i].cooldown-=delta;
        if(dragon_combat[i].windup>0) {
            dragon_combat[i].windup-=delta;
            if(dragon_combat[i].windup>0) return;
            typedef void *(__cdecl *Allocate)(size_t);
            void *memory=((Allocate)(game_image+0xc0706))(0x6c);
            if(!memory) return;
            BOOL previous=creating_fireball;
            creating_fireball=TRUE;
            void *projectile=construct_fireball(memory,world,1);
            if(projectile) {
                void **world_vtable=*(void ***)world;
                ((ActionFunction)world_vtable[0x20/4])(world,projectile);
                void **vtable=*(void ***)projectile;
                ((ActionFunction)vtable[0x3c/4])(projectile,world);
                typedef void (__attribute__((thiscall)) *Launch)(void *,void *,int);
                ((Launch)vtable[0x50/4])(projectile,entity+0x10,dragon_combat[i].direction);
                custom_trace("dragon fireball launched");
            }
            creating_fireball=previous;
            return;
        }
        if(dragon_combat[i].cooldown>0) return;
        int dx=*(int *)(hero+0x10)-*(int *)(entity+0x10);
        int dy=*(int *)(hero+0x14)-*(int *)(entity+0x14);
        if((dx && dy) || abs(dx)+abs(dy)>7 || (!dx && !dy)) return;
        // Native projectile directions use the game's own cell vectors.
        const int *vectors=(const int *)(game_image+0x148e10);
        for(int direction=0;direction<4;++direction) {
            if((dx && vectors[direction*2]==(dx>0?1:-1)) ||
               (dy && vectors[direction*2+1]==(dy>0?1:-1))) {
                dragon_combat[i].direction=direction;
                dragon_combat[i].animation=0;
                dragon_combat[i].windup=.28f;
                dragon_combat[i].cooldown=2.2f;
                custom_trace("dragon aiming at player");
                return;
            }
        }
    }
}

static void *__attribute__((thiscall)) find_restored_particle(void *theme, GameString *name)
{
    const char *alias=NULL;
    if(custom_running && restored_art==ART_BLOBBY && action_is(name,"genericZoogBlobTrail")) alias="hdBlobbyTrail";
    if(restored_art==ART_DRAGON && action_is(name,"fireZoogBlobTrail")) alias="hdDragonTrail";
    if(!alias)
        return original_find_particle(theme,name);
    GameString replacement={0};
    ((MakeString)(game_image+0x3d30))(&replacement,alias);
    void *particle=original_find_particle(theme,&replacement);
    ((PhaseFunction)(game_image+0x4f890))(&replacement);
    custom_trace(particle?alias:"missing monster trail");
    return particle ? particle : original_find_particle(theme,name);
}

static GameString *__attribute__((thiscall)) selected_editor_theme(void *self, GameString *result)
{
    (void)self;
    return ((MakeString)(game_image + 0x3d30))(result, custom_theme);
}

static void *open_mod_file(const char *path)
{
    typedef void *(__attribute__((thiscall)) *StreamCtor)(void *, int);
    typedef void (__attribute__((thiscall)) *StreamOpen)(void *, GameString *, GameString *);
    typedef void *(__cdecl *GameAlloc)(size_t);
    void *memory=((GameAlloc)(game_image+0xc0706))(0x18);
    if (!memory) return NULL;
    void *stream=((StreamCtor)(game_image+0xe1560))(memory,1);
    GameString empty={.capacity=15},name={0};
    ((MakeString)(game_image+0x3d30))(&name,path);
    ((StreamOpen)(game_image+0xe1690))(stream,&empty,&name);
    ((PhaseFunction)(game_image+0x4f890))(&name);
    return stream;
}

static BOOL is_custom_level_resource(const char *path)
{
    if (!custom_running || !path) return FALSE;
    for (const char *expected = custom_level_resource; ; ++expected, ++path) {
        char ch = *path == '\\' ? '/' : *path;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != *expected) return FALSE;
        if (!ch) return TRUE;
    }
}

static void *mod_raw_file(const char *path)
{
    if (is_custom_level_resource(path)) return open_mod_file(custom_path);
    char file[MAX_PATH];
    if (!iggle_mods_resource(path,custom_running,file)) return NULL;
    custom_trace(path);
    return open_mod_file(file);
}

static BYTE __attribute__((thiscall)) resource_exists_hook(void *self,const char *path)
{
    if (is_custom_level_resource(path)) return TRUE;
    char file[MAX_PATH];
    if (iggle_mods_resource(path,custom_running,file)) return TRUE;
    return original_resource_exists(self,path);
}

static void *__attribute__((thiscall)) raw_stream_hook(void *self,const char *path)
{
    void *stream=mod_raw_file(path);
    return stream?stream:original_raw_stream(self,path);
}

static void *__attribute__((thiscall)) archive_stream_hook(void *self,const char *path)
{
    void *stream=mod_raw_file(path);
    return stream?stream:original_archive_stream(self,path);
}

static void *__attribute__((thiscall)) open_stream_hook(void *manager, const char *path)
{
    // Native campaign levels must never inherit the editor's object registry.
    if(path && (!_strnicmp(path,"res/levels/dx/",14) || !_strnicmp(path,"res\\levels\\dx\\",14) ||
                !_strnicmp(path,"res/levels/retro/",17) || !_strnicmp(path,"res\\levels\\retro\\",17))) {
        custom_running=FALSE;loading_custom_objects=FALSE;
        reset_restored_objects();
        custom_trace("native campaign objects only");
        custom_trace(path);
    }

    if (is_custom_level_resource(path)) {
        typedef void *(__attribute__((thiscall)) *ParserStreamCtor)(void *,void *,int);
        typedef void *(__cdecl *GameAlloc)(size_t);
        void *adapter=((GameAlloc)(game_image+0xc0706))(0x18);
        void *stream=adapter?open_mod_file(custom_path):NULL;
        if (!stream) {
            if (adapter) ((void (__cdecl *)(void *))(game_image+0xc0c51))(adapter);
            return NULL;
        }
        custom_trace(path);
        custom_trace("disk file open");
        return ((ParserStreamCtor)(game_image+0xa7910))(adapter,stream,1);
    }
    return original_open_stream(manager,path);
}

static void __attribute__((thiscall)) custom_init_hook(void *state)
{
    if (state != custom_state) { original_test_init(state); return; }
    BYTE *game = *(BYTE **)((BYTE *)state + 0x0c);
    void **editor = (void **)(game + 0xb8);
    void *saved = *editor;
    void *theme_vtable[31] = {0};
    theme_vtable[30] = selected_editor_theme;
    void **theme_provider = theme_vtable;
    // The retail game omits the editor instance. Supply the selected theme
    // only while the original playtest state initializes.
    custom_trace("test state init");
    *editor = &theme_provider;
    original_test_init(state);
    *editor = saved;
    custom_trace("test state initialized");
}

static void restore_menu_input(BYTE *game)
{
    // The pause/continue manager can keep capturing clicks above the main menu.
    // Detach it as the native dialog transition does when its animation ends.
    void *dialogs = *(void **)(game_image+0x148c64);
    void *desktop = *(void **)(game+0x20);
    void *screens = *(void **)(game+0x28);
    if (dialogs && desktop) {
        void **vtable = *(void ***)desktop;
        ((ActionFunction)vtable[0x108/4])(desktop,dialogs);
    }
    if (screens) {
        void **vtable = *(void ***)screens;
        ((PhaseFunction)vtable[0x88/4])(screens);
    }
}


static void finish_custom(BYTE *game)
{
    reset_crystal_power();
    reset_restored_objects();
    custom_trace("custom gameplay finished");
    custom_exit_pending = FALSE;
    custom_completed = FALSE;
    *(DWORD *)(game+0x130) = saved_level_index;
    *(DWORD *)(game+0x134) = saved_level_number;
    custom_state = NULL;
    custom_game = NULL;
    custom_running = FALSE;
    custom_path[0] = 0;
    iggle_input_set_gameplay(FALSE);
    restore_menu_input(game);
    ((PhaseFunction)(game_image + 0x2ac0))(game);
    iggle_menu_load("mainMenu.xml");
    custom_trace("main menu queued");
}

static void __attribute__((thiscall)) custom_enter_hook(void *state)
{
    if (state != custom_state) { original_test_enter(state); return; }
    custom_trace("test state enter");
    if (!*((BYTE *)state + 0x10)) {
        reset_restored_objects();
        BYTE *game = *(BYTE **)((BYTE *)state + 0x0c);
        void *world = *(void **)(game + 0xd8);
        GameString path = {0};
        ((MakeString)(game_image + 0x3d30))(&path, custom_level_resource);
        void **vtable = *(void ***)world;
#ifdef IGGLE_CUSTOM_TEST
        void *exception_probe = AddVectoredExceptionHandler(1, trace_custom_exception);
#endif
        loading_custom_objects=TRUE;
        BYTE loaded = ((LoadLevel)vtable[0x38 / 4])(world, &path);
        loading_custom_objects=FALSE;
#ifdef IGGLE_CUSTOM_TEST
        RemoveVectoredExceptionHandler(exception_probe);
#endif
        ((PhaseFunction)(game_image + 0x4f890))(&path);
        if (custom_temporary[0]) {
            DeleteFileA(custom_temporary);
            custom_temporary[0] = 0;
        }
        custom_trace(loaded ? "custom XML loaded" : "custom XML rejected");
        if (!loaded) { finish_custom(game); return; }
    }
    original_test_enter(state);
    custom_trace("custom gameplay entered");
    iggle_menu_clear();
    iggle_input_set_gameplay(TRUE);
}

static BOOL action_is(void *action, const char *label)
{
    const char *object = action;
    unsigned length = *(const unsigned *)(object + 0x14);
    unsigned capacity = *(const unsigned *)(object + 0x18);
    if (length != strlen(label) || capacity < length) return FALSE;
    const char *text = capacity < 16 ? object + 4 : *(const char *const *)(object + 4);
    return text && memcmp(text, label, length) == 0;
}

static void queue_custom_exit(void);

static void __attribute__((thiscall)) custom_action_hook(void *state, void *action)
{
    if (state == custom_state) {
        if (action_is(action, "gameExit") || action_is(action, "gameNextLevel") ||
            action_is(action, "gamePrevLevel")) {
            queue_custom_exit();
            return;
        }
        if (action_is(action, "gameReloadLevel")) {
            *((BYTE *)state + 0x10) = 0;
            custom_enter_hook(state);
            return;
        }
    }
    original_test_action(state, action);
}

// These values belong to the native mixer channels, independent of the saved
// mute switches. Store the levels separately from the campaign profile.
static int audio_volume[2] = {100, 100};
static const char *const audio_keys[] = {"effects", "music"};
static char audio_settings_path[MAX_PATH];
static BOOL audio_volume_ready;
static void *active_audio_options;
static void (__attribute__((thiscall)) *original_options_init)(void *, void *, void *);
static BYTE (__attribute__((thiscall)) *original_load_profile)(void *);
static BYTE (__attribute__((thiscall)) *original_write_profile)(void *);
static ActionFunction original_record_campaign;

static void set_mixer_volume(int channel, BOOL enabled)
{
    if(channel==0)iggle_voice_volume(enabled?audio_volume[0]:0);
    void *mixer = *(void **)(game_image + 0x1499cc);
    if (!mixer) return;
    typedef void (__attribute__((thiscall)) *SetVolume)(void *, int, float);
    ((SetVolume)(*(void ***)mixer)[0x24 / 4])(mixer, channel,
        enabled ? audio_volume[channel] / 100.f : 0.f);
#ifdef IGGLE_CUSTOM_TEST
    char message[60];
    snprintf(message, sizeof message, "mixer channel %d volume %d", channel, enabled ? audio_volume[channel] : 0);
    custom_trace(message);
#endif
}

static void __attribute__((thiscall)) set_effects_enabled(void *self, BYTE enabled)
{
    (void)self;
    set_mixer_volume(0, enabled);
}

static void __attribute__((thiscall)) set_music_enabled(void *self, BYTE enabled)
{
    (void)self;
    set_mixer_volume(1, enabled);
}

static void *find_option(void *screen, const char *name)
{
    GameString key = {0};
    ((MakeString)(game_image + 0x3d30))(&key, name);
    typedef void *(__attribute__((thiscall)) *FindOption)(void *, GameString *);
    void *widget = ((FindOption)(game_image + 0x7dd60))(screen, &key);
    ((PhaseFunction)(game_image + 0x4f890))(&key);
    return widget;
}

static void refresh_volume_bars(void *screen)
{
    for (int channel = 0; channel < 2; ++channel) {
        const char *check_name = channel ? "musicOptionCheck" : "soundOptionCheck";
        void *check = find_option(screen, check_name);
        if (!check) continue;
        typedef BYTE (__attribute__((thiscall)) *IsChecked)(void *);
        BOOL enabled = ((IsChecked)(*(void ***)check)[0x15c / 4])(check);
        for (int step = 1; step <= 10; ++step) {
            char name[16];
            snprintf(name, sizeof name, "hdVol%dFill%d", channel, step);
            void *segment = find_option(screen, name);
            if (segment)
                ((ActionFunction)(game_image + 0x81df0))(segment, (void *)(intptr_t)(enabled && audio_volume[channel] >= step * 10));
        }
    }
}

static void __attribute__((thiscall)) initialize_audio_options(void *screen, void *first, void *second)
{
    original_options_init(screen, first, second);
    active_audio_options=screen;
    iggle_menu_load("optionMenu.xml");
    iggle_input_set_gameplay(FALSE);
    refresh_volume_bars(screen);
}

static BOOL handle_volume_option(void *screen, void *action)
{
    if (!audio_volume_ready) return FALSE;
    char name[16];
    for (int channel = 0; channel < 2; ++channel) {
        for (int step = 0; step <= 10; ++step) {
            snprintf(name, sizeof name, "hdVol%dStep%d", channel, step);
            if (!action_is(action, name)) continue;
            audio_volume[channel] = step * 10;
            char value[5];
            snprintf(value, sizeof value, "%d", audio_volume[channel]);
            if (*audio_settings_path) WritePrivateProfileStringA("audio", audio_keys[channel], value, audio_settings_path);
            const char *check_name = channel ? "musicOptionCheck" : "soundOptionCheck";
            void *check = find_option(screen, check_name);
            if (check) {
                typedef BYTE (__attribute__((thiscall)) *IsChecked)(void *);
                BOOL enabled = ((IsChecked)(*(void ***)check)[0x15c / 4])(check);
                if (!enabled && step > 0) {
                    ((ActionFunction)(*(void ***)check)[0x160 / 4])(check, (void *)1);
                    GameString toggle = {.length = 11, .capacity = 15};
                    memcpy(toggle.value.inline_text, channel ? "toggleMusic" : "toggleSound", 12);
                    original_options(screen, &toggle);
                    enabled = TRUE;
                }
                set_mixer_volume(channel, enabled);
            }
            refresh_volume_bars(screen);
            return TRUE;
        }
    }
    return FALSE;
}

static void __attribute__((thiscall)) record_campaign_progress(void *game, void *world)
{
    // The native playtest uses level 9999. Its exit must never update the
    // campaign's in-memory progression or write it to the player's profile.
    if ((custom_running && game == custom_game) || *(int *)((BYTE *)game + 0x134) == 9999) {
        custom_trace("custom progression excluded from campaign save");
        return;
    }
    original_record_campaign(game, world);
}

static BYTE __attribute__((thiscall)) repair_loaded_profile(void *profile)
{
    BYTE loaded = original_load_profile(profile);
    if (!loaded) return loaded;
    BYTE *data = profile;
    int *last = (int *)(data + 0x3c);
    int *highest = (int *)(data + 0x48);
    if (*highest != 9999) return loaded;
    BYTE *medals = *(BYTE **)(data + 0x5c);
    BYTE *end = *(BYTE **)(data + 0x60);
    if (!medals || !end || end - medals < 9999 || !medals[9998]) return loaded;
    // The last legitimate medal proves the next playable stage. A playtest
    // medal at index 9998 was the only out-of-range entry the mod generated.
    for (BYTE *entry = medals + 101; entry < end; ++entry)
        if (entry != medals + 9998 && *entry) return loaded;
    int recovered = 1;
    for (int i = 0; i < 101; ++i)
        if (medals[i]) recovered = i < 100 ? i + 2 : 101;
    medals[9998] = 0;
    *(BYTE **)(data + 0x60) = medals + 101;
    *highest = recovered;
    if (*last > recovered) *last = recovered;
    original_write_profile(profile);
    custom_trace("campaign progression repaired from medals");
    return loaded;
}

static void __attribute__((thiscall)) options_hook(void *self,void *action)
{
    if (handle_volume_option(self, action)) return;
    if (action_is(action, "hdControlOptions")) {
        PostMessageA(g_ddraw.hwnd, WM_APP+0x485,0,0);return;
    }
    if (action_is(action, "hdDisplayOptions")) {
        PostMessageA(g_ddraw.hwnd, WM_APP + 0x481, 0, 0);
        return;
    }
    BOOL closing_options = action_is(action, "done");
    BOOL toggling_audio = action_is(action, "toggleSound") || action_is(action, "toggleMusic");
#ifdef IGGLE_CUSTOM_TEST
    if (toggling_audio) custom_trace(action_is(action, "toggleMusic") ? "music checkbox action" : "effects checkbox action");
    if (closing_options) custom_trace("options closed by OK");
#endif
    original_options(self, action);
    if (audio_volume_ready && toggling_audio)
        refresh_volume_bars(self);
    if (closing_options) {
        active_audio_options=NULL;
        iggle_menu_load(in_pause_menu ? "pauseMenu.xml" : "mainMenu.xml");
    }
}

BOOL iggle_confirm_native_options(int x,int y)
{
    if (!active_audio_options) return FALSE;
    const char *name=NULL,*check_name=NULL;
    char step_name[16];
    if (y==162 || y==207) {
        BOOL music=y==207;
        if (x==425) {
            name=music?"toggleMusic":"toggleSound";
            check_name=music?"musicOptionCheck":"soundOptionCheck";
        } else if (x>=468 && x<=578) {
            snprintf(step_name,sizeof step_name,"hdVol%dStep%d",music,(x-468)/11);
            name=step_name;
        }
    } else if (x==463 && y==252) {
        name="toggleHelperArrows";
        check_name="helperArrowsCheck";
    } else if (x==345 && y==319) name="hdDisplayOptions";
    else if (x==523 && y==319) name="hdControlOptions";
    else if (x==432 && y==382) name="done";
    if (!name) return FALSE;
    if (check_name) {
        void *check=find_option(active_audio_options,check_name);
        if (!check) return FALSE;
        typedef BYTE (__attribute__((thiscall)) *IsChecked)(void *);
        BOOL checked=((IsChecked)(*(void ***)check)[0x15c/4])(check);
        ((ActionFunction)(*(void ***)check)[0x160/4])(check,(void *)(intptr_t)!checked);
    }
    GameString action={0};
    ((MakeString)(game_image+0x3d30))(&action,name);
    options_hook(active_audio_options,&action);
    ((PhaseFunction)(game_image+0x4f890))(&action);
    return TRUE;
}

static void __attribute__((thiscall)) main_hook(void *self,void *action)
{
#ifdef IGGLE_CUSTOM_TEST
    {GameString *label=action;custom_trace(label->capacity<16?label->value.inline_text:label->value.heap_text);}
#endif

    iggle_input_set_gameplay(FALSE);
    if (action_is(action,"hdMods")) {
        custom_menu_state=self;
        iggle_open_mods(self);
        return;
    }
    if (action_is(action, "hdLanguage")) {
        PostMessageA(g_ddraw.hwnd, WM_APP + 0x483, 0, 0);
        return;
    }
    if (action_is(action, "hdCustomLevels") || action_is(action, "hdLevelEditor") || action_is(action, "edit")) {
        custom_state = NULL;
        custom_game = NULL;
        custom_exit_pending = FALSE;
        custom_running = FALSE;
        custom_trace("custom menu action");
        custom_menu_state = self;
        PostMessageA(g_ddraw.hwnd, WM_APP + 0x489, 0, 0);
        return;
    }
    if(action_is(action,"newGame") || action_is(action,"newRetro")) {
        custom_running=FALSE;loading_custom_objects=FALSE;
        reset_crystal_power();
        reset_restored_objects();
        custom_state=NULL;custom_game=NULL;custom_exit_pending=FALSE;
        custom_trace("native game selected; restored objects disabled");
    }
    iggle_menu_load("mainMenu.xml");
    if (action_is(action,"options")) iggle_menu_load("optionMenu.xml");
    else if (action_is(action,"scores")) iggle_menu_load("highScoreScreen.xml");
    original_main(self, action);
}

static void queue_custom_exit(void)
{
    if (custom_exit_pending || !custom_game) return;
    in_pause_menu = FALSE;
    custom_exit_pending = TRUE;
    PostMessageA(g_ddraw.hwnd, WM_APP + 0x48a, 0, 0);
}

static void __attribute__((thiscall)) pause_action_hook(void *self, void *action)
{
#ifdef IGGLE_CUSTOM_TEST
    if (custom_running) {
        const GameString *command=action;
        custom_trace(command->capacity<16?command->value.inline_text:command->value.heap_text);
    }
#endif
    if (custom_running && custom_state && action_is(action, "pauseSaveQuit")) {
        custom_trace("leaving via pause");
        // The pause menu belongs to a separate native screen manager.
        // Resume dismisses its modal and releases the game's input capture.
        GameString resume = { .value.inline_text = "pauseResume", .length=11, .capacity=15 };
        original_pause_action(self,&resume);
        queue_custom_exit();
        return;
    }
    if (custom_running && custom_state && action_is(action, "levelEndDone")) {
        custom_completed = TRUE;
        queue_custom_exit();
        return;
    }
    if (action_is(action,"pauseOptions")) iggle_menu_load("optionMenu.xml");
    if (action_is(action, "pauseResume")) {
        in_pause_menu = FALSE;
        iggle_menu_clear();
        iggle_input_set_gameplay(TRUE);
    }
    original_pause_action(self, action);
}

static BOOL install_action(BYTE *target, const BYTE *prefix, size_t count,
                           ActionFunction replacement, ActionFunction *original)
{
    if (memcmp(target, prefix, count)) return FALSE;
    BYTE *trampoline = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!trampoline) return FALSE;
    memcpy(trampoline, target, count);
    trampoline[count] = 0xe9;
    int32_t relative = (int32_t)((intptr_t)(target + count) - (intptr_t)(trampoline + count + 5));
    memcpy(trampoline + count + 1, &relative, 4);
    DWORD protection;
    if (!VirtualProtect(trampoline, 32, PAGE_EXECUTE_READ, &protection) ||
        !VirtualProtect(target, count, PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return FALSE;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, count + 5);
    *original = (ActionFunction)trampoline;
    relative = (int32_t)((intptr_t)replacement - (intptr_t)(target + 5));
    target[0] = 0xe9;
    memcpy(target + 1, &relative, 4);
    memset(target + 5, 0x90, count - 5);
    VirtualProtect(target, count, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), target, count);
    return TRUE;
}

static void install_restored_objects(BYTE *base)
{
    const BYTE slow[] = {0x6a,0xff,0x68,0x0b,0x24,0x50,0x00};
    const BYTE rainbow[] = {0x6a,0xff,0x68,0x04,0x51,0x50,0x00};
    const BYTE dragon[] = {0x6a,0xff,0x68,0x77,0x52,0x50,0x00};
    const BYTE spawn[] = {0x6a,0xff,0x68,0x69,0x23,0x50,0x00};
    const BYTE show[] = {0x6a,0xff,0x68,0xb8,0x24,0x50,0x00};
    const BYTE reset[] = {0x6a,0xff,0x68,0xee,0x24,0x50,0x00};
    const BYTE collision[] = {0x56,0x8b,0xf1,0x8b,0x4e,0x40};
    const BYTE art[] = {0x56,0x57,0x8b,0x7c,0x24,0x0c};
    const BYTE particle_lookup[] = {0x64,0xa1,0,0,0,0};
    const BYTE projectile_constructor[] = {0x6a,0xff,0x68,0x89,0x3b,0x50,0x00};
    const BYTE projectile_art[] = {0x6a,0xff,0x68,0xbe,0x3c,0x50,0x00};
    const BYTE projectile_other_hit[] = {0x8b,0x44,0x24,0x04,0x56,0x8b,0xf1};
    const BYTE projectile_player_hit[] = {0x56,0x8b,0xf1,0x8b,0x4c,0x24,0x08};
    const BYTE player_step[] = {0x8b,0x81,0xe0,0x00,0x00,0x00};
    const BYTE fuzzle_cleanup[] = {0x6a,0xff,0x68,0x94,0x59,0x50,0x00};
    const BYTE cage_entity[] = {0x6a,0xff,0x68,0x18,0x50,0x50,0x00};
    const BYTE cage_show[] = {0x6a,0xff,0x68,0x83,0x50,0x50,0x00};
    const BYTE cage_attach[] = {0x56,0x57,0x8b,0x7c,0x24,0x0c};
    const BYTE trapped[] = {0x6a,0xff,0x68,0x03,0x5a,0x50,0x00};
    const BYTE dragon_entity[] = {0x6a,0xff,0x68,0x43,0x51,0x50,0x00};
    const BYTE dragon_activate[] = {0x6a,0xff,0x68,0xc3,0x51,0x50,0x00};
    const BYTE dragon_create[] = {0x6a,0xff,0x68,0x10,0x52,0x50,0x00};
    const BYTE monster_update[] = {0x6a,0xff,0x68,0xc3,0x51,0x50,0x00};
    const BYTE blobby_init[] = {0x6a,0xff,0x68,0x8f,0x44,0x50,0x00};
    const BYTE monster_tick[] = {0x8b,0x49,0x4c,0x8b,0x41,0x54};
    const BYTE freed[] = {0x6a,0xff,0x68,0x65,0x5a,0x50,0x00};
    const BYTE dragon_init[] = {0x6a,0xff,0x68,0x52,0x3f,0x50,0x00};
    const BYTE make_sprite[] = {0x6a,0xff,0x68,0x24,0x83,0x50,0x00};
    const BYTE sprite_draw[] = {0x56,0x8b,0xf1,0x8b,0x06};
    const BYTE collect_jewel[]={0x51,0x56,0x68,0,0,0x80,0x3f};
    const BYTE lethal[] = {0x6a,0xff,0x68,0xbb,0x33,0x50,0x00};
    BOOL hit_hook=install_action(base+0x4b510,lethal,sizeof lethal,(ActionFunction)fatal_hit,(ActionFunction *)&original_fatal_hit);
    BOOL direct_hit_hook=install_action(base+0x4b460,lethal,sizeof lethal,(ActionFunction)direct_fatal_hit,(ActionFunction *)&original_direct_fatal_hit);
    const BYTE world_lethal[] = {0x64,0xa1,0,0,0,0};
    BOOL world_hit_hook=install_action(base+0x339b0,world_lethal,sizeof world_lethal,
        world_fatal_hit,&original_world_fatal_hit);
    crystal_shield_ready=hit_hook && direct_hit_hook && world_hit_hook;
    jewel_ready=install_action(base+0x338d0,collect_jewel,sizeof collect_jewel,(ActionFunction)collect_native_jewel,(ActionFunction *)&original_collect_jewel);
    const BYTE world_step[] = {0x83,0xec,0x08,0x55,0x8b,0x6c,0x24,0x10};
    if (!install_action(base+0x24ae0, art, sizeof art, (ActionFunction)find_restored_art, (ActionFunction *)&original_find_art)) return;
    install_action(base+0x24b30, particle_lookup, sizeof particle_lookup,
        (ActionFunction)find_restored_particle, (ActionFunction *)&original_find_particle);
    install_action(base+0x50060, projectile_constructor, sizeof projectile_constructor,
        (ActionFunction)construct_fireball, (ActionFunction *)&original_projectile_constructor);
    install_action(base+0x50930, projectile_art, sizeof projectile_art, draw_fireball_art, &original_projectile_art);
    install_action(base+0x50570, projectile_other_hit, sizeof projectile_other_hit, fireball_other_hit, &original_projectile_other_hit);
    install_action(base+0x50540, projectile_player_hit, sizeof projectile_player_hit, fireball_player_hit, &original_fireball_player_hit);
    install_action(base+0x4a820, player_step, sizeof player_step, step_dragon_slow_player, &original_player_step);
    if (!install_action(base+0x63f00,fuzzle_cleanup,sizeof fuzzle_cleanup,
        (ActionFunction)forget_rainbow_fuzzle,(ActionFunction *)&original_fuzzle_cleanup)) return;
    install_action(base+0x5e0a0, rainbow, sizeof rainbow, (ActionFunction)rainbow_spawner, (ActionFunction *)&original_rainbow_spawner);
    install_action(base+0x5eb10, dragon, sizeof dragon, (ActionFunction)dragon_spawner, (ActionFunction *)&original_dragon_spawner);
    install_action(base+0x5da00, cage_entity, sizeof cage_entity, (ActionFunction)spawn_cage, (ActionFunction *)&original_spawn_cage);
    install_action(base+0x5dc50, cage_show, sizeof cage_show, show_cage, &original_cage_show);
    install_action(base+0x60930, cage_attach, sizeof cage_attach, attach_cage, &original_attach_cage);
    const BYTE breakfree_voice[]={0x6a,0xff,0x68,0x30,0x57,0x50,0x00};
    install_action(base+0x631a0,breakfree_voice,sizeof breakfree_voice,
        (ActionFunction)voiced_fuzzle_breakfree,(ActionFunction *)&original_fuzzle_breakfree);
    install_action(base+0x641a0, trapped, sizeof trapped, show_trapped_fuzzle, &original_trapped_art);
    install_action(base+0x5e420, dragon_entity, sizeof dragon_entity, (ActionFunction)spawn_monster, (ActionFunction *)&original_spawn_monster);
    install_action(base+0x5e780, dragon_activate, sizeof dragon_activate, activate_dragon, &original_dragon_activate);
    install_action(base+0x5e970, dragon_create, sizeof dragon_create, create_dragon, &original_dragon_create);
    install_action(base+0x5e670, monster_update, sizeof monster_update, update_monster_spawner, &original_spawn_monster_update);
    install_action(base+0x519e0, dragon_init, sizeof dragon_init, init_dragon_art, &original_dragon_init);
    install_action(base+0x559d0, blobby_init, sizeof blobby_init, init_blobby_art, &original_blobby_init);
    install_action(base+0x522e0, monster_tick, sizeof monster_tick, tick_monster_art, &original_monster_tick);
    install_action(base+0x64520, freed, sizeof freed, free_rainbow_fuzzle, &original_freed_fuzzle);
    install_action(base+0x82e50, make_sprite, sizeof make_sprite,
        (ActionFunction)make_monster_sprite, (ActionFunction *)&original_make_sprite);
    install_action(base+0x82dd0, sprite_draw, sizeof sprite_draw, draw_restored_monster, &original_monster_draw);
    if (install_action(base+0x3aa80, world_step, sizeof world_step, step_restored_time, &original_world_step) &&
        install_action(base+0x41e00, collision, sizeof collision, pickup_collision, &original_pickup_collision) &&
        install_action(base+0x41bf0, spawn, sizeof spawn, (ActionFunction)spawn_pickup, (ActionFunction *)&original_spawn_pickup)) {
        install_action(base+0x42240, show, sizeof show, pickup_show, &original_pickup_show);
        install_action(base+0x42770, reset, sizeof reset, pickup_reset, &original_pickup_reset);
        install_action(base+0x42090, slow, sizeof slow, (ActionFunction)slow_spawner, (ActionFunction *)&original_slow_spawner);
    }
}

int iggle_current_language(void);

static void patch_game_bytes(BYTE *target, const void *expected, const void *replacement, size_t size)
{
    if (memcmp(target, expected, size)) return;
    DWORD protection;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &protection)) return;
    memcpy(target, replacement, size);
    VirtualProtect(target, size, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), target, size);
}

static BOOL replace_native_audio_toggle(BYTE *target, const BYTE *expected, ActionFunction replacement)
{
    if (memcmp(target, expected, 5)) return FALSE;
    BYTE jump[5] = {0xe9};
    int32_t distance = (BYTE *)replacement - (target + 5);
    memcpy(jump + 1, &distance, sizeof distance);
    patch_game_bytes(target, expected, jump, sizeof jump);
    return memcmp(target, jump, sizeof jump) == 0;
}

static void __attribute__((thiscall)) attach_ship_clouds(void *maze, int layer, void *clouds)
{
    typedef void (__attribute__((thiscall)) *Attach)(void *, int, void *);
    typedef void *(__attribute__((thiscall)) *FindImage)(void *, GameString *);
    typedef void *(__cdecl *GameAlloc)(size_t);
    Attach attach = (Attach)(game_image + 0x17c20);
    void *theme = *(void **)((BYTE *)maze + 0x23c);
    if (theme) {
        GameString name = {.value.inline_text = "wideSky", .length = 7, .capacity = 15};
        void *image = ((FindImage)(game_image + 0x24ae0))(theme, &name);
        void *memory = image ? ((GameAlloc)(game_image + 0xc0706))(0x128) : NULL;
        if (memory) {
            void *sky = ((SpawnPickup)(game_image + 0x82e50))(memory, image, 1);
            typedef void (__attribute__((thiscall)) *Position)(void *, float, float);
            void **methods = *(void ***)sky;
            ((Position)methods[0x18 / sizeof(void *)])(sky, 374.f, 243.f);
            // Layer zero is behind the existing moving clouds and the deck.
            attach(maze, 0, sky);
            custom_trace("ship sky attached behind clouds");
        }
    }
    attach(maze, layer, clouds);
}

static void install_ship_background(BYTE *base)
{
    const BYTE attach_underfx[] = {0xe8, 0x6a, 0x61, 0xfe, 0xff};
    BYTE replacement[5] = {0xe8};
    BYTE *callsite = base + 0x31ab1;
    int32_t relative = (BYTE *)attach_ship_clouds - (callsite + sizeof replacement);
    memcpy(replacement + 1, &relative, sizeof relative);
    patch_game_bytes(callsite, attach_underfx, replacement, sizeof replacement);
}

static void install_audio_and_save_guards(BYTE *base)
{
    const BYTE save_progress[] = {0x6a,0xff,0x68,0xe3,0xd3,0x4f,0};
    const BYTE load_profile[] = {0x6a,0xff,0x68,0x2d,0x08,0x50,0};
    const BYTE write_profile[] = {0x6a,0xff,0x68,0x4c,0x07,0x50,0};
    install_action(base + 0x52b0, save_progress, sizeof save_progress,
                   record_campaign_progress, &original_record_campaign);
    if (!memcmp(base + 0x27780, write_profile, sizeof write_profile)) {
        original_write_profile = (void *)(base + 0x27780);
        install_action(base + 0x28450, load_profile, sizeof load_profile,
                       (ActionFunction)repair_loaded_profile, (ActionFunction *)&original_load_profile);
    }

    char app_data[MAX_PATH];
    DWORD app_data_length = GetEnvironmentVariableA("LOCALAPPDATA", app_data, sizeof app_data);
    if (app_data_length && app_data_length < sizeof app_data - 25) {
        char directory[MAX_PATH];
        snprintf(directory, sizeof directory, "%s\\IgglePopHD", app_data);
        CreateDirectoryA(directory, NULL);
#ifdef IGGLE_CUSTOM_TEST
        const char *audio_filename = "audio-test.ini";
#else
        const char *audio_filename = "audio.ini";
#endif
        snprintf(audio_settings_path, sizeof audio_settings_path, "%s\\%s", directory, audio_filename);
    }
    for (int channel = 0; channel < 2; ++channel) {
        UINT volume = GetPrivateProfileIntA("audio", audio_keys[channel], 100, audio_settings_path);
        audio_volume[channel] = volume <= 100 ? volume : 100;
    }
    iggle_voice_volume(audio_volume[0]);
    const BYTE effects[] = {0xe8,0x9b,0xb8,0xff,0xff};
    const BYTE music[] = {0xe8,0xdb,0xb8,0xff,0xff};
    const BYTE options_init[] = {0x6a,0xff,0x68,0x90,0x8a,0x50,0};
    audio_volume_ready = replace_native_audio_toggle(base + 0x7bab0, effects, (ActionFunction)set_effects_enabled) &&
                         replace_native_audio_toggle(base + 0x7ba70, music, (ActionFunction)set_music_enabled);
    if (audio_volume_ready)
        install_action(base + 0x8a960, options_init, sizeof options_init,
                       (ActionFunction)initialize_audio_options, (ActionFunction *)&original_options_init);
}

static void localize_game_notices(BYTE *base)
{
    const int language = iggle_current_language();
    // These animations start at the first letter and advance 36 units per letter.
    // Shift the entire animation by half the added width of the playable field.
    const float oldX = 80.f, wideX = 192.f;
    const float oldShortX = 120.f, wideShortX = 232.f;
    if (*(const DWORD *)(base + 0x468b) == 864) {
        patch_game_bytes(base + 0x35084, &oldX, &wideX, sizeof(float));
        patch_game_bytes(base + 0x34d97, &oldX, &wideX, sizeof(float));
        patch_game_bytes(base + 0x353fe, &oldShortX, &wideShortX, sizeof(float));
    }
    // The original font arrays have 256 entries, but five readers sign-extend char.
    // Latin-1 accents must index 128..255 instead of reading before those arrays.
    const BYTE signedChar = 0xbe, unsignedChar = 0xb6;
    const DWORD fontReaders[] = {0xa31b1, 0xa31e4, 0xa3291, 0xa32b1, 0xa32e1};
    for (unsigned i = 0; i < sizeof fontReaders / sizeof fontReaders[0]; ++i)
        patch_game_bytes(base + fontReaders[i], &signedChar, &unsignedChar, 1);
    if (language == 0) return;
    static const char *ready[] = {"READY", "PREPARAR", "PREPARADOS"};
    static const char *set[] = {"SET", "APONTAR", "LISTOS"};
    static const char *go[] = {"GO!", "J\xc1!", "YA!"};
    const DWORD lengthOffsets[] = {0x34a86,0x391c9,0x39183,0x39122};
    const DWORD pointerOffsets[] = {0x34a8d,0x391cb,0x39185,0x39124};
    const DWORD originalRvas[] = {0x110f6c,0x110f6c,0x11117c,0x111180};
    const BYTE lengths[] = {5,5,3,3};
    const char *translated[] = {ready[language],ready[language],set[language],go[language]};
    for(unsigned i=0;i<4;i++){
        DWORD expected=(DWORD)(base+originalRvas[i]);
        DWORD replacement=(DWORD)translated[i];
        BYTE count=(BYTE)strlen(translated[i]);
        if(*(DWORD *)(base+pointerOffsets[i])==expected && base[lengthOffsets[i]]==lengths[i]){
            patch_game_bytes(base+lengthOffsets[i],&lengths[i],&count,1);
            patch_game_bytes(base+pointerOffsets[i],&expected,&replacement,4);
        }
    }
    patch_game_bytes(base + 0x11510c, "Level:%d", language == 1 ? "Fase: %d" : "Nivel:%d", 8);
    patch_game_bytes(base + 0x10f3e0, "Level:", language == 1 ? "Fase: " : "Nivel:", 6);
    patch_game_bytes(base + 0x10f3b0, "Platinum", language == 1 ? "Platina " : "Platino ", 8);
    patch_game_bytes(base + 0x10f3bc, "Gold", language == 1 ? "Ouro" : "Oro ", 4);
    patch_game_bytes(base + 0x10f3c4, "Silver", language == 1 ? "Prata " : "Plata ", 6);
    patch_game_bytes(base + 0x10f3cc, "Bronze", language == 1 ? "Bronze" : "Bronce", 6);
    patch_game_bytes(base + 0x110fa8, "ALL CLEAR!", language == 1 ? "CONCLUIDO!" : "SUPERADO! ", 10);
    patch_game_bytes(base + 0x110f80, "GAME OVER!", language == 1 ? "FIM JOGO! " : "FIN!      ", 10);
    patch_game_bytes(base + 0x110fb4, "SORRY!", language == 1 ? "OPA!  " : "UPS!  ", 6);
    patch_game_bytes(base + 0x110e68, "CHAIN BONUS!\\n500", language == 1 ? "BONUS FILA!\\n500 " : "BONO FILA!\\n500  ", 17);
    patch_game_bytes(base + 0x110e8c, "Secret!", language == 1 ? "Segredo" : "Secreto", 7);
    patch_game_bytes(base + 0x110e94, "1-Up!", language == 1 ? "+VIDA" : "+VIDA", 5);
    patch_game_bytes(base + 0x110fc8, "Chain\\nShrink!", language == 1 ? "Fila\\nMenor!  " : "Fila\\nCorta!  ", 14);
    patch_game_bytes(base + 0x110fe8, "Auto\\nSort!", language == 1 ? "Ordenar!   " : "Ordenar!   ", 11);
}

void iggle_install_game_hook(void)
{
    static BOOL attempted;
    if (attempted) return;
    attempted = TRUE;
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage != 0x15c000 ||
        memcmp(base + 0x11797c, "toggleFullScreen", 16)) return;
#ifdef IGGLE_CUSTOM_TEST
    if(GetEnvironmentVariableA("IGGLE_STORY_DIAGNOSTIC",NULL,0)) {
        game_image=base;
        AddVectoredExceptionHandler(1,trace_custom_exception);
        const BYTE art_probe[]={0x56,0x57,0x8b,0x7c,0x24,0x0c};
        install_action(base+0x24ae0,art_probe,sizeof art_probe,(ActionFunction)find_restored_art,(ActionFunction*)&original_find_art);
        custom_trace("diagnostic: original gameplay hooks");
        return;
    }
#endif
    localize_game_notices(base);
    const BYTE layout_prefix[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0xf0,0x7c,0x50,0x00};
    const BYTE pause_action_prefix[] = {0x64,0xa1,0,0,0,0};
    ActionFunction layout_trampoline = NULL;
    if (install_action(base + 0x7da20, layout_prefix, sizeof layout_prefix,
                       (ActionFunction)layout_hook, &layout_trampoline))
        original_layout_loader = (LoadLayout)layout_trampoline;
    const BYTE queue_screen_prefix[]={0x64,0xa1,0,0,0,0};
    install_action(base + 0x7a4e0,queue_screen_prefix,sizeof queue_screen_prefix,
                   (ActionFunction)queue_screen_hook,(ActionFunction *)&original_queue_screen);
    install_action(base + 0xe110, pause_action_prefix, sizeof pause_action_prefix,
                   pause_action_hook, &original_pause_action);

    game_image = base;
#ifdef IGGLE_CUSTOM_TEST
    AddVectoredExceptionHandler(1,trace_custom_exception);
#endif
    const BYTE test_init[] = {0x6a,0xff,0x68,0x86,0xd6,0x4f,0x00};
    const BYTE test_finish[] = {0x64,0xa1,0,0,0,0};
    const BYTE stream_prefix[] = {0x8b,0x89,0x48,0x01,0x00,0x00};
    ActionFunction init = NULL, enter = NULL, stream = NULL;
    BOOL init_ok = install_action(base + 0x7830, test_init, sizeof test_init, (ActionFunction)custom_init_hook, &init);
    BOOL enter_ok = install_action(base + 0x7c40, test_finish, sizeof test_finish, (ActionFunction)custom_enter_hook, &enter);
    BOOL action_ok = install_action(base + 0x9ba0, test_finish, sizeof test_finish, custom_action_hook, &original_test_action);
    BOOL stream_ok = install_action(base + 0x77420, stream_prefix, sizeof stream_prefix, (ActionFunction)open_stream_hook, &stream);
    install_restored_objects(base);
    original_test_init = (PhaseFunction)init;
    original_test_enter = (PhaseFunction)enter;
    original_open_stream = (OpenStream)stream;
    const BYTE raw_prefix[]={0x6a,0xff,0x68,0x18,0x96,0x50,0x00};
    const BYTE archive_prefix[]={0x6a,0xff,0x68,0xf8,0x95,0x50,0x00};
    BOOL raw_ok=install_action(base+0x96d10,raw_prefix,sizeof raw_prefix,
        (ActionFunction)raw_stream_hook,(ActionFunction *)&original_raw_stream);
    BOOL archive_ok=install_action(base+0x96c00,archive_prefix,sizeof archive_prefix,
        (ActionFunction)archive_stream_hook,(ActionFunction *)&original_archive_stream);
    const BYTE exists_prefix[]={0x6a,0xff,0x68,0x18,0x96,0x50,0x00};
    BOOL exists_ok=install_action(base+0x96ee0,exists_prefix,sizeof exists_prefix,
        (ActionFunction)resource_exists_hook,(ActionFunction *)&original_resource_exists);
    if (raw_ok && archive_ok && exists_ok) iggle_mods_startup();
    custom_ready = init_ok && enter_ok && action_ok && stream_ok;
    if (*(const DWORD *)(base + 0x468b) == 864) install_ship_background(base);
    const BYTE retro_prefix[] = {0x6a,0xff,0x68,0xbb,0x19,0x50,0x00};
    const BYTE bonus_prefix[] = {0x6a,0xff,0x68,0xff,0x0d,0x50,0x00};
    const BYTE update_prefix[] = {0x56,0x8b,0xf1,0x8a,0x86,0x4d,0x01,0x00,0x00};
    install_action(base+0x3b400,retro_prefix,sizeof retro_prefix,(ActionFunction)retro_hook,&original_retro);
    install_action(base+0x30b20,bonus_prefix,sizeof bonus_prefix,(ActionFunction)bonus_hook,&original_bonus);
    install_action(base+0x31570,update_prefix,sizeof update_prefix,active_update_hook,&original_active_update);
    const BYTE phase_prefix[] = {0x6a,0xff,0x68,0xa6,0x0e,0x50,0x00};
    install_action(base+0x31880, phase_prefix, sizeof phase_prefix, (ActionFunction)phase_hook, &original_phase);
    const BYTE options_prefix[] = {0x64, 0xa1, 0, 0, 0, 0};
    const BYTE main_prefix[] = {0x6a, 0xff, 0x68, 0x04, 0xd5, 0x4f, 0x00};
    install_action(base + 0x8a660, options_prefix, sizeof options_prefix, options_hook, &original_options);
    install_action(base + 0x59d0, main_prefix, sizeof main_prefix, main_hook, &original_main);
    install_audio_and_save_guards(base);
}

int iggle_current_language(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%siggle-hd\\language.ini", g_config.game_path);
    int value = GetPrivateProfileIntA("language", "index", 0, path);
    return value >= 0 && value <= 2 ? value : 0;
}

BOOL iggle_select_language(HWND hwnd, int selection)
{
    if (selection < 1 || selection > 3) return FALSE;
    wchar_t folder[MAX_PATH], helper[MAX_PATH], command[MAX_PATH + 64], parameters[64];
    if (!MultiByteToWideChar(CP_ACP, 0, g_config.game_path, -1, folder, MAX_PATH) ||
        wcslen(folder) + 32 >= MAX_PATH) return FALSE;
    swprintf(helper, MAX_PATH, L"%lsIgglePop-Language.exe", folder);
    const wchar_t *languages[] = { L"en", L"pt-BR", L"es" };
    swprintf(parameters, 64, L"--language %lu %ls", GetCurrentProcessId(), languages[selection - 1]);
    swprintf(command, MAX_PATH + 64, L"\"%ls\" --language %lu %ls",
        helper, GetCurrentProcessId(), languages[selection - 1]);
    wchar_t probe[MAX_PATH];
    swprintf(probe, MAX_PATH, L"%lsIgglePop.saf", folder);
    HANDLE writable = CreateFileW(probe, GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD accessError = writable == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (writable != INVALID_HANDLE_VALUE) CloseHandle(writable);
    if (accessError == ERROR_ACCESS_DENIED)
        return (INT_PTR)ShellExecuteW(hwnd, L"runas", helper, parameters, folder, SW_SHOWNORMAL) > 32;
    STARTUPINFOW startup = { sizeof startup };
    PROCESS_INFORMATION process = {0};
    BOOL started = CreateProcessW(helper, command, NULL, NULL, FALSE, 0, NULL, folder, &startup, &process);
    if (started) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    return started;
}

BOOL iggle_custom_launch(void *menu)
{
    if (!menu || menu != custom_menu_state || !custom_ready || !iggle_custom_get_path(custom_path)) return FALSE;
    BYTE *game = *(BYTE **)((BYTE *)menu + 0x0c);
    if (!game) return FALSE;
    if (!iggle_level_prepare(custom_path, custom_temporary)) return FALSE;
    typedef void *(__cdecl *GameAlloc)(size_t);
    void *memory = ((GameAlloc)(game_image + 0xc0706))(0x1c);
    if (!memory) {
        if (custom_temporary[0]) DeleteFileA(custom_temporary);
        custom_temporary[0] = 0;
        return FALSE;
    }
    memset(memory, 0, 0x1c);
    custom_trace("custom level selected");
    custom_menu_state = NULL;
    saved_level_index = *(DWORD *)(game+0x130);
    saved_level_number = *(DWORD *)(game+0x134);
    custom_state = ((BuildState)(game_image + 0x20f0))(memory, game, 1);
    custom_game = game;
    custom_running = TRUE;
    custom_theme = iggle_level_theme(custom_path);
    custom_trace(custom_theme);
    custom_init_hook(custom_state);
    void *queue = game + 0x178;
    void **vtable = *(void ***)queue;
    ((QueueState)vtable[0x14 / 4])(queue, custom_state);
    return TRUE;
}

void iggle_leave_custom_level(void)
{
    if (!custom_exit_pending || !custom_game) return;
    BYTE *game = custom_game;
    custom_exit_pending = FALSE;
    void *world = *(void **)(game + 0xd8);
    if (world && !custom_completed) {
        ((PhaseFunction)(game_image + 0x308c0))(world);
        ((PhaseFunction)(game_image + 0x2cfa0))(world);
    }
    finish_custom(game);
}

void iggle_show_custom_levels(void)
{
    if (custom_menu_state) iggle_open_custom_levels(custom_menu_state);
}
