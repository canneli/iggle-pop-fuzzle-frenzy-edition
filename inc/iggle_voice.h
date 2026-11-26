#ifndef IGGLE_VOICE_H
#define IGGLE_VOICE_H
#include <windows.h>
typedef struct { BOOL alive, trapped, following; float x,y; } IggleVoiceActor;
typedef struct { float x,y,opening,progress; unsigned direction,event; BOOL active,mouth_visible; } IggleVoiceView;
typedef BOOL (*IggleVoiceProbe)(void *,IggleVoiceActor *);
void iggle_voice_reset(void);
void iggle_voice_stop(void);
void iggle_voice_volume(unsigned percent);
void iggle_voice_register(void *actor);
void iggle_voice_forget(void *actor);
void iggle_voice_lost(void *actor);
void *iggle_voice_build(void *actor);
void iggle_voice_art(void *image,const char *name);
void iggle_voice_sprite(void *sprite,void *image);
void iggle_voice_draw(void *sprite);
void iggle_voice_tick(float seconds,IggleVoiceProbe probe);
BOOL iggle_voice_view(IggleVoiceView *view);
BOOL iggle_voice_available(void);
#endif
