// User-authored dialogue. No native actor state is modified by this module.
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"
#include "config.h"
#include "iggle_custom_levels.h"
#include "iggle_voice.h"

enum { VOICE_HEY,VOICE_LOST,VOICE_WAIT,VOICE_HELP,VOICE_ACTORS=1024,VOICE_SPRITES=8192 };
static struct { void *actor; IggleVoiceActor state; BOOL sampled,was_following; } actors[VOICE_ACTORS];
static struct { void *sprite,*actor; unsigned direction; } sprites[VOICE_SPRITES];
static unsigned actor_count,sprite_count,volume=100,rng;
static void *building,*speaking,*lost_actor;
static void *build_image;
static unsigned build_direction;
static float age,cooldown,next_help,lost_age,last_lost;
static unsigned initial_trapped,max_trapped,help_count;
static BOOL greeted,warned;
static HWAVEOUT output;
static WAVEHDR header;
static BYTE *wave_bytes;
static unsigned byte_rate;
static SRWLOCK view_lock=SRWLOCK_INIT;
static IggleVoiceView mouth_view;
static DWORD visible_at;
static DWORD view_expires;
static const char *names[]={"hey","lost","wait","help"};

static void trace(const char *event,unsigned kind) {
#ifdef IGGLE_CUSTOM_TEST
 FILE *f=fopen("voice-smoke.log","a");
 if(f){fprintf(f,"%s %s lang=%d age=%.2f actors=%u\n",event,names[kind],iggle_current_language(),age,actor_count);fclose(f);}
#else
 (void)event;(void)kind;
#endif
}
static unsigned random_index(unsigned count) {
 if(!rng)rng=GetTickCount()^0x46555a5a;
 rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
 return count?rng%count:0;
}
static void hide_mouth(void) {
 AcquireSRWLockExclusive(&view_lock);
 memset(&mouth_view,0,sizeof mouth_view);visible_at=view_expires=0;
 ReleaseSRWLockExclusive(&view_lock);
}
void iggle_voice_stop(void) {
 speaking=NULL;
 if(output){waveOutReset(output);waveOutUnprepareHeader(output,&header,sizeof header);waveOutClose(output);output=NULL;}
 if(wave_bytes){free(wave_bytes);wave_bytes=NULL;}
 memset(&header,0,sizeof header);hide_mouth();
}
void iggle_voice_reset(void) {
 iggle_voice_stop();actor_count=sprite_count=0;building=build_image=lost_actor=NULL;
 age=cooldown=lost_age=0;next_help=28;initial_trapped=max_trapped=help_count=0;greeted=warned=FALSE;
 last_lost=-100;
}
void iggle_voice_volume(unsigned percent) {
 volume=min(percent,100u);
 if(output){DWORD level=volume*65535u/100u;waveOutSetVolume(output,level|(level<<16));}
 if(!volume)iggle_voice_stop();
}
void iggle_voice_register(void *actor) {
 if(!actor)return;
 for(unsigned i=0;i<actor_count;++i)if(actors[i].actor==actor)return;
 if(actor_count<VOICE_ACTORS){memset(&actors[actor_count],0,sizeof actors[0]);actors[actor_count++].actor=actor;}
}
void iggle_voice_forget(void *actor) {
 // Delivering the speaker must not chop a sentence halfway through.
 if(speaking==actor){speaking=NULL;hide_mouth();}
 if(lost_actor==actor)lost_actor=NULL;
 for(unsigned i=0;i<actor_count;)if(actors[i].actor==actor)actors[i]=actors[--actor_count];else ++i;
 for(unsigned i=0;i<sprite_count;)if(sprites[i].actor==actor)sprites[i]=sprites[--sprite_count];else ++i;
}
void iggle_voice_lost(void *actor) {
 // Signalled by the native break-free vocal event, never by home delivery
 // or a normal player-assisted rescue. Do not change the escape mechanics.
 iggle_voice_register(actor);lost_actor=actor;lost_age=0;
}
void *iggle_voice_build(void *actor) {void *previous=building;building=actor;build_image=NULL;return previous;}
void iggle_voice_art(void *image,const char *name) {
 if(!building||!image||!name)return;
 size_t n=strlen(name);unsigned direction=4;
 if(!_strnicmp(name,"trapped",7)&&n>7)direction=name[n-1]=='0'?0:3;
 else if(!_strnicmp(name,"follow",6)||!_strnicmp(name,"hdRainbow",9)) {
  char c=name[n-1];direction=c=='S'||c=='s'?0:c=='E'||c=='e'?1:c=='W'||c=='w'?2:3;
 }
 if(direction<4){build_image=image;build_direction=direction;}
}
void iggle_voice_sprite(void *sprite,void *image) {
 if(!building||!sprite||image!=build_image)return;
 unsigned i=0;while(i<sprite_count&&sprites[i].sprite!=sprite)++i;
 if(i>=VOICE_SPRITES)return;
 if(i==sprite_count)++sprite_count;
 sprites[i].sprite=sprite;sprites[i].actor=building;sprites[i].direction=build_direction;
}
void iggle_voice_draw(void *sprite) {
 if(!speaking)return;
 for(unsigned i=0;i<sprite_count;++i)if(sprites[i].sprite==sprite&&sprites[i].actor==speaking) {
  AcquireSRWLockExclusive(&view_lock);
  mouth_view.direction=sprites[i].direction;visible_at=GetTickCount();
  ReleaseSRWLockExclusive(&view_lock);return;
 }
}
static BOOL read_wave(const char *path,WAVEFORMATEX *format) {
 FILE *f=fopen(path,"rb");if(!f)return FALSE;
 fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);
 if(length<32||length>4*1024*1024){fclose(f);return FALSE;}
 BYTE *bytes=malloc(length);
 if(!bytes){fclose(f);return FALSE;}
 BOOL ok=fread(bytes,1,length,f)==(size_t)length;fclose(f);
 if(!ok||memcmp(bytes,"OggS",4)){free(bytes);return FALSE;}
 short *pcm=NULL;int channels=0,rate=0;
 int samples=stb_vorbis_decode_memory(bytes,(int)length,&channels,&rate,&pcm);free(bytes);
 if(samples<=0||channels!=1||rate<16000||rate>48000||samples>rate*15){free(pcm);return FALSE;}
 memset(format,0,sizeof *format);format->wFormatTag=WAVE_FORMAT_PCM;format->nChannels=1;
 format->nSamplesPerSec=rate;format->wBitsPerSample=16;format->nBlockAlign=2;format->nAvgBytesPerSec=rate*2;
 wave_bytes=(BYTE *)pcm;header.lpData=(LPSTR)pcm;header.dwBufferLength=samples*2;byte_rate=format->nAvgBytesPerSec;return TRUE;
}
BOOL iggle_voice_available(void) {
 char path[MAX_PATH];const char *lang=iggle_current_language()==1?"pt-BR":"en";
 snprintf(path,sizeof path,"%siggle-hd\\voices\\%s\\hey.ogg",g_config.game_path,lang);
 return GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES;
}
static BOOL play(unsigned kind,void *actor) {
 if(!volume||!actor||output)return FALSE;
 char path[MAX_PATH];const char *lang=iggle_current_language()==1?"pt-BR":"en";
 snprintf(path,sizeof path,"%siggle-hd\\voices\\%s\\%s.ogg",g_config.game_path,lang,names[kind]);
 WAVEFORMATEX format;
 if(!read_wave(path,&format)){trace("missing",kind);return FALSE;}
 if(waveOutOpen(&output,WAVE_MAPPER,&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR){output=NULL;iggle_voice_stop();return FALSE;}
 iggle_voice_volume(volume);
 if(waveOutPrepareHeader(output,&header,sizeof header)!=MMSYSERR_NOERROR||waveOutWrite(output,&header,sizeof header)!=MMSYSERR_NOERROR){iggle_voice_stop();return FALSE;}
 speaking=actor;cooldown=8.f;hide_mouth();
 AcquireSRWLockExclusive(&view_lock);
 mouth_view.event=kind;mouth_view.active=TRUE;mouth_view.direction=3;
 view_expires=GetTickCount()+(DWORD)((uint64_t)header.dwBufferLength*1000/byte_rate)+60;
 for(unsigned i=0;i<actor_count;++i)if(actors[i].actor==actor){mouth_view.x=actors[i].state.x;mouth_view.y=actors[i].state.y;break;}
 ReleaseSRWLockExclusive(&view_lock);trace("play",kind);return TRUE;
}
static void *pick_trapped(void) {
 unsigned n=0;void *pick=NULL;
 for(unsigned i=0;i<actor_count;++i)if(actors[i].state.alive&&actors[i].state.trapped&&random_index(++n)==0)pick=actors[i].actor;
 return pick;
}
void iggle_voice_tick(float seconds,IggleVoiceProbe probe) {
 if(!probe||!(seconds>0&&seconds<.5f))return;
 age+=seconds;cooldown=max(0.f,cooldown-seconds);lost_age+=seconds;
 unsigned trapped=0;
 for(unsigned i=0;i<actor_count;++i) {
  IggleVoiceActor state={0};
  if(!probe(actors[i].actor,&state))continue;
  actors[i].state=state;actors[i].sampled=TRUE;actors[i].was_following=state.following;
  if(state.alive&&state.trapped)++trapped;
  if(actors[i].actor==speaking) {
   if(!state.alive){speaking=NULL;hide_mouth();continue;}
   AcquireSRWLockExclusive(&view_lock);mouth_view.x=state.x;mouth_view.y=state.y;ReleaseSRWLockExclusive(&view_lock);
  }
 }
 max_trapped=max(max_trapped,trapped);
 if(output) {
  if(header.dwFlags&WHDR_DONE)iggle_voice_stop();
  else {
   MMTIME clock={.wType=TIME_BYTES};
   if(waveOutGetPosition(output,&clock,sizeof clock)==MMSYSERR_NOERROR) {
    DWORD cursor=clock.wType==TIME_BYTES?clock.u.cb:clock.wType==TIME_SAMPLES?clock.u.sample*2:clock.wType==TIME_MS?clock.u.ms*byte_rate/1000:header.dwBufferLength;
    cursor=min(cursor,header.dwBufferLength);cursor&=~1u;
    DWORD end=min(header.dwBufferLength,cursor+byte_rate/50);double sum=0;unsigned count=0;
    for(DWORD p=cursor;p+1<end;p+=2){int16_t sample;memcpy(&sample,header.lpData+p,2);sum+=(double)sample*sample;++count;}
    float rms=count?(float)sqrt(sum/count)/32768.f:0;
    AcquireSRWLockExclusive(&view_lock);mouth_view.opening=rms<.012f?0:min(1.f,(rms-.012f)*7.f);mouth_view.progress=header.dwBufferLength?cursor/(float)header.dwBufferLength:1.f;ReleaseSRWLockExclusive(&view_lock);
   }
  }
  return;
 }
 if(!greeted&&age>=.8f&&trapped){initial_trapped=trapped;greeted=TRUE;play(VOICE_HEY,pick_trapped());return;}
 if(!volume)return;
 if(lost_actor&&lost_age>.25f&&lost_age<6.f&&age-last_lost>=10.f){
  void *who=lost_actor;lost_actor=NULL;
  for(unsigned i=0;i<actor_count;++i)if(actors[i].actor==who&&actors[i].state.alive&&!actors[i].state.following&&!actors[i].state.trapped){if(play(VOICE_LOST,who))last_lost=age;return;}
 }
 unsigned threshold=max(1u,min(3u,initial_trapped/5));
 if(!warned&&age>=8&&max_trapped>threshold&&trapped>0&&trapped<=threshold){warned=TRUE;play(VOICE_WAIT,pick_trapped());return;}
 if(cooldown>0)return;
 if(age>=next_help&&trapped>threshold&&help_count<2){++help_count;next_help=age+40.f+random_index(16);play(VOICE_HELP,pick_trapped());}
}
BOOL iggle_voice_view(IggleVoiceView *view) {
 AcquireSRWLockShared(&view_lock);*view=mouth_view;DWORD visible=visible_at,expires=view_expires;ReleaseSRWLockShared(&view_lock);
 view->mouth_visible=visible&&(DWORD)(GetTickCount()-visible)<100&&view->direction<3&&view->opening>.01f;
 return view->active && (LONG)(expires-GetTickCount())>0;
}
