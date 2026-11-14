#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <math.h>
#include <d3d9.h>
#include "dd.h"
#include "hook.h"
#include "config.h"
#include "utils.h"
#include "mouse.h"
#include "winapi_hooks.h"
#include "iggle_glyphs.h"
#include "iggle_custom_levels.h"
#include "iggle_level_editor.h"
#include "iggle_menu.h"
#include "iggle_mods.h"

static volatile LONG opened, hovered = -1;
BOOL iggle_editor_is_open(void) { return InterlockedCompareExchange(&opened,0,0)==5; }
BOOL iggle_overlay_is_open(void) { return InterlockedCompareExchange(&opened,0,0)!=0; }
static int choice;
static int mod_page, mod_count, mod_notice;
static int custom_total, custom_page;
static void *custom_menu;
static BOOL custom_error;
static int library_selection, library_confirm;
static wchar_t editor_path[MAX_PATH];
static int editor_tool, editor_color, editor_x, editor_y, editor_side, editor_notice, editor_confirm;
static BOOL editor_copy;
static int editor_picker, picker_focus, editor_monster, editor_power;
static BOOL editor_panel_focus;
static int editor_panel_item;
#ifdef IGGLE_CUSTOM_TEST
static void trace_editor(const char *message, int a, int b)
{
 FILE *file=fopen("custom-smoke.log","a");
 if (file) { fprintf(file,"%s %d %d\n",message,a,b); fclose(file); }
}
#else
#define trace_editor(message, a, b) ((void)0)
#endif

static volatile LONG language_feedback;
int iggle_current_language(void);
BOOL iggle_select_language(HWND, int);
static DWORD suppress_until;
struct Art { const char *name; IDirect3DTexture9 *tex; int w,h; };
static struct Art art[] = {{"menuwindowlarge"},{"orangebutton"},{"orangebuttonhi"},{"dialogfont"},{"sqbuttonbasenormal"},{"sqbuttonbasehi"},
 {"level-hero"},{"level-zoog"},
 {"level-cage-green"},{"level-cage-blue"},{"level-cage-yellow"},{"level-cage-purple"},
 {"level-home-green"},{"level-home-blue"},{"level-home-yellow"},{"level-home-purple"},
 #define WORLD_ART(world) {"level-" world "-floortile0"},{"level-" world "-floortile1"}, \
                          {"level-" world "-hwallsegment"},{"level-" world "-vwallsegment"}
 WORLD_ART("editor"),WORLD_ART("snow"),WORLD_ART("desert"),WORLD_ART("cave"),WORLD_ART("ship")
 #undef WORLD_ART
 ,{"level-blobby"},{"level-shooter"},{"level-armor"},{"level-fire"},{"level-tunnel"},
 {"level-guard"},{"level-bomber"},{"level-gmz"},
 {"level-freeze"},{"level-zap"},{"level-sort"},
 {"level-pop-green"},{"level-pop-blue"},{"level-pop-yellow"},{"level-pop-purple"},
 {"level-slow"},{"level-dragon"},{"level-rainbow-0"},{"level-rainbow-1"},
 {"editor-hudbg"},{"editor-modeselbtn"},{"editor-modeselbtnhi"},{"editor-modeselbtnchecked"},
 {"editor-editbtnbase"},{"editor-editbtnbasehi"},{"editor-imgframe"},{"editor-tinybtnbase"},{"rainbow-diamond"},{"rainbow-aura"},{"crystal-sparkle"},{"mod-floor"},{"mod-filled"},{"mod-wall-h"},{"mod-wall-v"}
};
struct Vertex { float x,y,z,rhw; DWORD color; float u,v; };

void iggle_overlay_release(void) {
 for(int i=0;i<ARRAYSIZE(art);i++) if(art[i].tex) { IDirect3DTexture9_Release(art[i].tex);art[i].tex=NULL; }
}
static BOOL load_art(IDirect3DDevice9 *device,int i) {
 char path[MAX_PATH];
 FILE *f = NULL;
 if (!strncmp(art[i].name,"mod-",4)) {
  const char *names[]={"mod-floor","mod-filled","mod-wall-h","mod-wall-v"};
  static char last_path[4][MAX_PATH];
  int tile=0;
  while(tile<4 && strcmp(art[i].name,names[tile])) ++tile;
  if (tile==4 || !iggle_mods_preview(tile,path)) return FALSE;
  if (art[i].tex && strcmp(last_path[tile],path)) {
   IDirect3DTexture9_Release(art[i].tex);
   art[i].tex=NULL;
  }
  if(art[i].tex)return TRUE;
  f=fopen(path,"rb");
  if (!f) return FALSE;
  strcpy(last_path[tile],path);
 } else if(art[i].tex)return TRUE;
 if (strcmp(art[i].name, "level-freeze") == 0) {
  int lang = iggle_current_language();
  const char *prefix = lang == 1 ? "pt-BR" : lang == 2 ? "es" : NULL;
  if (prefix) {
   snprintf(path, sizeof(path), "%siggle-hd\\%s-%s.rgba", g_config.game_path, prefix, art[i].name);
   f = fopen(path, "rb");
  }
 }
 if (!f) {
  snprintf(path, sizeof(path), "%siggle-hd\\%s.rgba", g_config.game_path, art[i].name);
  f = fopen(path, "rb");
 }
 if (!f) return FALSE;
 int w=0,h=0;fread(&w,4,1,f);fread(&h,4,1,f);
 if(w<1||h<1||w>4096||h>4096){fclose(f);return FALSE;}
 IDirect3DTexture9 *tex=NULL;
 if(FAILED(IDirect3DDevice9_CreateTexture(device,w,h,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex,NULL))){fclose(f);return FALSE;}
 D3DLOCKED_RECT lock;
 if(FAILED(IDirect3DTexture9_LockRect(tex,0,&lock,NULL,0))){IDirect3DTexture9_Release(tex);fclose(f);return FALSE;}
 BOOL ok=TRUE;for(int y=0;y<h;y++) if(fread((BYTE*)lock.pBits+y*lock.Pitch,4,w,f)!=(size_t)w){ok=FALSE;break;}
 IDirect3DTexture9_UnlockRect(tex,0);fclose(f);
 if(!ok){IDirect3DTexture9_Release(tex);return FALSE;}
 art[i].tex=tex;art[i].w=w;art[i].h=h;return TRUE;
}
static void quad(IDirect3DDevice9*d,IDirect3DTexture9*t,float x,float y,float w,float h,DWORD color,float u,float v,float du,float dv){
 struct Vertex q[]={{x-.5f,y-.5f,0,1,color,u,v},{x+w-.5f,y-.5f,0,1,color,u+du,v},{x-.5f,y+h-.5f,0,1,color,u,v+dv},{x+w-.5f,y+h-.5f,0,1,color,u+du,v+dv}};
 IDirect3DDevice9_SetTexture(d,0,(IDirect3DBaseTexture9*)t);
 IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,q,sizeof(q[0]));
}
static float ui_scale(void){return min(g_ddraw.render.viewport.width/640.f,g_ddraw.render.viewport.height/480.f);}
static float ui_x(void){return g_ddraw.render.viewport.x+(g_ddraw.render.viewport.width-640.f*ui_scale())*.5f;}
static float ui_y(void){return g_ddraw.render.viewport.y+(g_ddraw.render.viewport.height-480.f*ui_scale())*.5f;}
static void image(IDirect3DDevice9*d,int i,float x,float y,float w,float h){
 quad(d,art[i].tex,ui_x()+x*ui_scale(),ui_y()+y*ui_scale(),w*ui_scale(),h*ui_scale(),0xffffffff,0,0,1,1);
}
static const struct Glyph *glyph(int ch){ for(unsigned i=0;i<sizeof(glyphs)/sizeof(glyphs[0]);i++)if(glyphs[i].ch==ch)return &glyphs[i];return NULL; }
static void text(IDirect3DDevice9*d,const char*s,float center,float y,float size){
 float width=0;for(const char*p=s;*p;p++){const struct Glyph*g=glyph((unsigned char)*p);if(g)width+=(g->a+g->b+g->c)*size;}
 float x=center-width/2;float sx=ui_scale(),sy=sx;
 for(;*s;s++){const struct Glyph*g=glyph((unsigned char)*s);if(!g)continue;x+=g->a*size;
 quad(d,art[3].tex,ui_x()+x*sx,ui_y()+y*sy,g->b*size*sx,26*size*sy,0xffffffff,(float)g->x/art[3].w,(float)g->y/art[3].h,(float)g->b/art[3].w,26.f/art[3].h);x+=(g->b+g->c)*size;}
}
enum { DISPLAY_FIRST_Y = 130, DISPLAY_ROW_STEP = 37, DISPLAY_ROW_HEIGHT = 33,
       DISPLAY_FSR_ROW = 5, DISPLAY_DONE_ROW = 6, DISPLAY_DONE_Y = 357 };

static void toggle_fsr(void)
{
 g_config.d3d9_filter = g_config.d3d9_filter == 4 ? 3 : 4;
 WritePrivateProfileStringA("ddraw", "d3d9_filter", g_config.d3d9_filter == 4 ? "4" : "3", g_config.ini_path);
}

enum { CUSTOM_PLAY=5, CUSTOM_EDIT, CUSTOM_NEW, CUSTOM_DELETE, CUSTOM_BACK, CUSTOM_PREV, CUSTOM_NEXT };
enum { EDIT_COMMAND_SAVE, EDIT_COMMAND_TEST, EDIT_COMMAND_BACK, EDIT_COMMAND_COLOR, EDIT_COMMAND_COPY, EDIT_COMMAND_WORLD, EDIT_COMMAND_PREVIOUS, EDIT_COMMAND_NEXT };
enum { ART_HERO=6, ART_ZOOG, ART_CAGE, ART_HOME=ART_CAGE+EDIT_COLOR_COUNT,
       ART_WORLDS=ART_HOME+EDIT_COLOR_COUNT, ART_PER_WORLD=4,
       ART_BLOBBY=ART_WORLDS+ART_PER_WORLD*EDIT_WORLD_COUNT, ART_SHOOTER, ART_ARMOR, ART_FIRE, ART_TUNNEL,
       ART_GUARD, ART_BOMBER, ART_GMZ, ART_FREEZE, ART_ZAP, ART_SORT, ART_POP,
       ART_SLOW=ART_POP+EDIT_COLOR_COUNT, ART_DRAGON, ART_RAINBOW, ART_RAINBOW_ALT,
       ART_EDITOR_HUD, ART_MODE, ART_MODE_HOVER, ART_MODE_CHECK, ART_EDITOR_BUTTON,
       ART_EDITOR_BUTTON_HOVER, ART_EDITOR_FRAME, ART_EDITOR_ARROW, ART_DIAMOND, ART_AURA, ART_CRYSTAL_SPARKLE, ART_MOD_TILES };
static const int monster_art[EDIT_ZOOG_COUNT] = {
 ART_ZOOG,ART_ZOOG,ART_BLOBBY,ART_BLOBBY,ART_SHOOTER,ART_ARMOR,ART_FIRE,ART_TUNNEL,ART_ZOOG,
 ART_GUARD,ART_GUARD,ART_BOMBER,ART_BOMBER,ART_BOMBER,ART_BOMBER,ART_BOMBER,ART_GMZ,ART_DRAGON
};
static const char *const monster_words[3][EDIT_ZOOG_COUNT] = {
 {"Tracker","Fast tracker","Blobby","Fast blobby","Shooter","Armored","Fire","Tunneler","Super Zoog","Guard","Fast guard","Slow bomber","Slow random","Fast bomber","Fast random","Hyper bomber","GMZ","Dragon"},
 {"Rastreador","Rastreador veloz","Gosmento","Gosmento veloz","Atirador","Blindado","Fogo","Escavador","Super Zoog","Guarda","Guarda veloz","Bombardeiro lento","Bombas aleat. lentas","Bombardeiro veloz","Bombas aleat. velozes","Hiperbombardeiro","GMZ","Drag\xe3o"},
 {"Rastreador","Rastreador veloz","Baboso","Baboso veloz","Tirador","Blindado","Fuego","Excavador","Super Zoog","Guardia","Guardia veloz","Bombardero lento","Bombas aleat. lentas","Bombardero veloz","Bombas aleat. veloces","Hiperbombardero","GMZ","Drag\xf3n"}
};
static const char *const power_words[3][EDIT_POWER_COUNT] = {
 {"Freeze","Zoog Zapper","Auto Sort","Fuzzle Pop","Slow pickup"},
 {"Congelar","Choque Zoog","Separar cores","Iggle Pop","Tempo lento"},
 {"Congelar","Electrochoque Zoog","Separar colores","Iggle Pop","Tiempo lento"}
};
static const char *const power_title[]={"Powers","Poderes","Poderes"};
static const char *const monster_title[]={"Monsters","Monstros","Monstruos"};
static const char *const world_words[3][EDIT_WORLD_COUNT] = {
 {"Garden","Snow","Desert","Cave","Spaceship"},
 {"Jardim","Neve","Deserto","Caverna","Nave"},
 {"Jard\xedn","Nieve","Desierto","Cueva","Nave"}
};
static const char *const library_words[3][10] = {
 {"Custom Levels","Play","Edit","New","Delete","Back","Create a level to begin.","Could not open this level.","Delete the selected level?","Cancel"},
 {"Fases personalizadas","Jogar","Editar","Nova","Apagar","Voltar","Crie uma fase para come\xe7" "ar.","Falha ao abrir esta fase.","Apagar a fase selecionada?","Cancelar"},
 {"Niveles personalizados","Jugar","Editar","Nuevo","Borrar","Volver","Crea un nivel para empezar.","No se pudo abrir este nivel.","\xbf" "Borrar el nivel seleccionado?","Cancelar"}
};
static const char *const editor_words[3][16] = {
 {"Wall","Floor","Hero","Cage","Home","Zoog","Erase","Save","Play","Levels","Color","Save a copy","Saved","Could not save","Discard unsaved changes?","Editing level"},
 {"Parede","Piso","Her\xf3i","Gaiola","Casa","Zoog","Borracha","Salvar","Testar","Fases","Cor","Salvar c\xf3pia","Salva","Falha ao salvar","Descartar as altera\xe7\xf5" "es?","Editar fase"},
 {"Muro","Suelo","H\xe9roe","Jaula","Casa","Zoog","Borrar","Guardar","Probar","Niveles","Color","Guardar copia","Guardado","No se pudo guardar","\xbf" "Descartar los cambios?","Editar nivel"}
};

static int custom_visible(void)
{
 int remaining = custom_total - custom_page * IGGLE_LEVEL_PAGE_SIZE;
 return max(0, min(remaining, IGGLE_LEVEL_PAGE_SIZE));
}

static struct Box { int x, y, w, h; } library_actions[] = {
 {156,310,75,30},{239,310,75,30},{322,310,75,30},{405,310,75,30},
 {268,353,104,39},{156,353,65,39},{419,353,65,39}
};
static const struct Box editor_actions[] = {
 {481,389,65,26},{481,421,65,30},{552,421,65,30},
 {481,317,136,26},{552,389,65,26},{481,228,136,24},
 {481,270,22,22},{595,270,22,22}
};
static struct Box editor_tool_box(int tool)
{
 static const int order[]={0,6,1,2,3,4,7,5};
 return (struct Box){481,35+24*order[tool],136,22};
}

static const struct Box confirm_yes = {181,249,128,35}, confirm_no = {331,249,128,35};

static BOOL inside(struct Box box, float x, float y)
{
 return x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h;
}

static void menu_button(IDirect3DDevice9 *d, struct Box button, const char *title, int highlight, BOOL green)
{
 image(d, green ? (highlight ? 5 : 4) : (highlight ? 2 : 1), button.x, button.y, button.w, button.h);
 text(d, title, button.x+button.w*.5f, button.y+(button.h-16)*.5f, .71f);
}

static void rectangle(IDirect3DDevice9 *d, float x, float y, float w, float h, DWORD color)
{
 quad(d,NULL,ui_x()+x*ui_scale(),ui_y()+y*ui_scale(),w*ui_scale(),h*ui_scale(),color,0,0,1,1);
}

static void draw_confirm(IDirect3DDevice9 *d, const char *message, const char *yes, const char *no, int focus)
{
 rectangle(d,0,0,640,480,0xa0001625);
 image(d,0,140,164,360,154);
 text(d,message,320,201,.83f);
 menu_button(d,confirm_yes,yes,focus==0,FALSE);
 menu_button(d,confirm_no,no,focus==1,TRUE);
}

static void draw_custom_levels(IDirect3DDevice9 *d)
{
 const int lang=iggle_current_language();
 const char *const *labels=library_words[lang];
 int over=InterlockedCompareExchange(&hovered,0,0);
 text(d,labels[0],320,77,1.13f);
 if (!custom_total) text(d,labels[6],320,203,.76f);
 for (int i=0;i<custom_visible();++i) {
  struct Box row={153,121+36*i,334,29};
  menu_button(d,row,iggle_custom_name(custom_page*IGGLE_LEVEL_PAGE_SIZE+i),library_selection==i || over==i,FALSE);
 }
 for (int i=CUSTOM_PLAY;i<=CUSTOM_BACK;++i) {
  if (!custom_total && (i==CUSTOM_PLAY || i==CUSTOM_EDIT || i==CUSTOM_DELETE)) continue;
  menu_button(d,library_actions[i-CUSTOM_PLAY],labels[i-CUSTOM_PLAY+1],over==i,i==CUSTOM_BACK);
 }
 if (custom_total>IGGLE_LEVEL_PAGE_SIZE) {
  char page[24];
  snprintf(page,sizeof page,"%d / %d",custom_page+1,(custom_total+IGGLE_LEVEL_PAGE_SIZE-1)/IGGLE_LEVEL_PAGE_SIZE);
  menu_button(d,library_actions[CUSTOM_PREV-CUSTOM_PLAY],"<",over==CUSTOM_PREV,FALSE);
  menu_button(d,library_actions[CUSTOM_NEXT-CUSTOM_PLAY],">",over==CUSTOM_NEXT,FALSE);
  text(d,page,320,398,.65f);
 }
 if (custom_error) text(d,labels[7],320,405,.67f);
 if (library_confirm) draw_confirm(d,labels[8],labels[4],labels[9],library_confirm-1);
}

static float editor_geometry(const EditorBoard *board, float *left, float *top)
{
 float size=board->columns && board->rows ? min(416.f/board->columns,390.f/board->rows) : 0;
 *left=34+(416-board->columns*size)/2;
 *top=47+(390-board->rows*size)/2;
 return size;
}

static int picker_count(void)
{
 return editor_picker==EDIT_ZOOG ? EDIT_ZOOG_COUNT : EDIT_POWER_COUNT;
}

static struct Box picker_box(int index)
{
 return (struct Box){98+149*(index%3),111+39*(index/3),145,32};
}

static int power_art(int type,int color)
{
 return type==POWER_POP ? ART_POP+color : type==POWER_SLOW ? ART_SLOW : ART_FREEZE+type;
}

static void draw_power_icon(IDirect3DDevice9 *d, int sprite, float x, float y, float size)
{
 if (sprite != ART_FREEZE || !art[sprite].w) {
  image(d,sprite,x,y,size,size);
  return;
 }
 float height = size * art[sprite].h / art[sprite].w;
 image(d,sprite,x,y+(size-height)*.5f,size,height);
}

static void draw_editor_picker(IDirect3DDevice9 *d,int lang)
{
 BOOL monsters=editor_picker==EDIT_ZOOG;
 rectangle(d,0,0,640,480,0xab001625);
 image(d,0,77,45,486,376);
 text(d,monsters?monster_title[lang]:power_title[lang],320,67,1.05f);
 for(int i=0;i<picker_count();++i) {
  struct Box slot=picker_box(i);
  image(d,picker_focus==i?2:1,slot.x,slot.y,slot.w,slot.h);
  if (monsters && monster_art[i]==ART_BLOBBY) image(d,ART_BLOBBY,slot.x+5,slot.y+1,33,30);
  else if (monsters) image(d,monster_art[i],slot.x+8,slot.y+3,25,25);
  else draw_power_icon(d,power_art(i,editor_color),slot.x+8,slot.y+3,25);
  text(d,monsters?monster_words[lang][i]:power_words[lang][i],slot.x+82,slot.y+8,.57f);
 }
 menu_button(d,(struct Box){265,355,110,37},library_words[lang][5],picker_focus==picker_count(),TRUE);
}

static void draw_level_editor(IDirect3DDevice9 *d)
{
 int lang=iggle_current_language();
 const char *const *labels=editor_words[lang];
 rectangle(d,11,4,455,472,0xff121c12);
 image(d,ART_EDITOR_HUD,469,0,158,480);
 text(d,labels[15],240,18,.98f);
 const char *mode[]={"Edit mode","Modo de edi\xe7\xe3o","Modo de edici\xf3n"};
 text(d,mode[lang],549,12,.68f);
 EditorBoard board;
 iggle_editor_board(&board);
 float left,top,step=editor_geometry(&board,&left,&top);
 int world_art=ART_WORLDS+min(board.world,EDIT_WORLD_COUNT-1)*ART_PER_WORLD;
 char mod_world[IGGLE_MOD_NAME]="";
 if (board.world==EDIT_WORLD_COUNT && iggle_mods_world(NULL,mod_world)) {
  BOOL ready=TRUE;
  for(int i=0;i<ART_PER_WORLD;++i) ready &= load_art(d,ART_MOD_TILES+i);
  if (ready) world_art=ART_MOD_TILES;
 }
 if (step>0) {
  rectangle(d,left-4,top-4,board.columns*step+8,board.rows*step+8,0xff315a28);
  for (int y=0;y<board.rows;++y) for (int x=0;x<board.columns;++x) {
   float px=left+x*step,py=top+y*step;
   image(d,world_art+!!(board.tiles[y][x]&EDIT_HAS_FLOOR),px,py,step,step);
   rectangle(d,px,py,step,1,0x3a000000);
   rectangle(d,px,py,1,step,0x3a000000);
  }
  for (int y=0;y<=board.rows;++y) for (int x=0;x<=board.columns;++x) {
   float px=left+x*step,py=top+y*step;
   unsigned tile=board.tiles[y][x];
   if ((tile&EDIT_HAS_WALL_H) && x<board.columns) image(d,world_art+2,px,py-step*.24f,step,step*.48f);
   if ((tile&EDIT_HAS_WALL_V) && y<board.rows) image(d,world_art+3,px-step*.20f,py,step*.40f,step);
   if (x>=board.columns || y>=board.rows) continue;
   unsigned object=board.objects[y][x];
   int sprite=-1;
   if (object==EDIT_HERO) sprite=ART_HERO;
   if (object==EDIT_ZOOG) sprite=monster_art[board.monsters[y][x]];
   if (object==EDIT_CAGE) sprite=board.colors[y][x]==EDIT_RAINBOW ? ART_RAINBOW+(GetTickCount()/420%2) : ART_CAGE+board.colors[y][x];
   if (object==EDIT_HOME) sprite=ART_HOME+board.colors[y][x];
   unsigned power=board.powers[y][x];
   if (power) {
    float width=step*(sprite>=0?.43f:.82f), offset=step*(sprite>=0?.52f:.09f);
    draw_power_icon(d,power_art(power-1,board.power_colors[y][x]),px+offset,py+offset,width);
   }
   if (sprite>=0) {
    float width=step*(power?.68f:.82f);
    image(d,sprite,px+step*.09f,py+step*.09f,width,width);
   }
  }
  float x=left+editor_x*step,y=top+editor_y*step;
  if (editor_tool==EDIT_WALL) {
   if (editor_side%2) rectangle(d,x+(editor_side==3?step-2:0),y,2,step,0xffffe282);
   else rectangle(d,x,y+(editor_side==2?step-2:0),step,2,0xffffe282);
  } else {
   rectangle(d,x,y,step,step,0x49ffe998);
  }
 }
 for (int i=0;i<EDIT_TOOL_COUNT;++i) {
  struct Box tool=editor_tool_box(i);
  const char *label=i==EDIT_ZOOG?monster_title[lang]:i==EDIT_POWER?power_title[lang]:labels[i];
  if (editor_panel_focus && hovered==i) rectangle(d,tool.x-2,tool.y-2,tool.w+4,tool.h+4,0xff8bdbff);
  image(d,i==editor_tool?ART_MODE_CHECK:i==hovered?ART_MODE_HOVER:ART_MODE,tool.x,tool.y,tool.w,tool.h);
  text(d,label,545,tool.y+3,.63f);
 }
 image(d,ART_EDITOR_FRAME,528,255,42,42);
 int preview=-1;
 if (editor_tool==EDIT_ZOOG) preview=monster_art[editor_monster];
 if (editor_tool==EDIT_POWER) preview=power_art(editor_power,editor_color);
 if (editor_tool==EDIT_CAGE) preview=editor_color==EDIT_RAINBOW?ART_RAINBOW+(GetTickCount()/420%2):ART_CAGE+editor_color;
 if (editor_tool==EDIT_HOME) preview=ART_HOME+editor_color;
 if (editor_tool==EDIT_HERO) preview=ART_HERO;
 if (editor_tool==EDIT_FLOOR) preview=world_art+1;
 if (editor_tool==EDIT_WALL) preview=world_art+2;
 if (preview==ART_FREEZE) draw_power_icon(d,preview,533,260,32);
 else if (preview>=0) image(d,preview,533,260,32,32);
 static const DWORD color_swatches[]={0xff1dbc3e,0xff208bfb,0xfff4d72c,0xffa842d7};
 int colors=editor_tool==EDIT_CAGE?EDIT_COLOR_COUNT+1:EDIT_COLOR_COUNT;
 for (int i=0;i<colors;++i) {
  int x=489+i*25;
  if (editor_panel_focus && editor_panel_item==EDIT_TOOL_COUNT+ARRAYSIZE(editor_actions)+i)
   rectangle(d,x-2,321,24,21,0xff8bdbff);
  rectangle(d,x,323,20,17,i==editor_color?0xffffff93:0xff0b1d0b);
  if(i==EDIT_RAINBOW) image(d,ART_RAINBOW+(GetTickCount()/420%2),x+2,325,16,13);
  else rectangle(d,x+2,325,16,13,color_swatches[i]);
 }
 const char *files[]={"Level files","Arquivos da fase","Archivos del nivel"};
 text(d,files[lang],549,364,.72f);
 for (int i=0;i<ARRAYSIZE(editor_actions);++i) {
  if (i==EDIT_COMMAND_COLOR) continue;
  struct Box action=editor_actions[i];
  BOOL arrow=i==EDIT_COMMAND_PREVIOUS || i==EDIT_COMMAND_NEXT;
  if (arrow && editor_tool!=EDIT_ZOOG && editor_tool!=EDIT_POWER && editor_tool!=EDIT_CAGE && editor_tool!=EDIT_HOME) continue;
  const char *label=i==EDIT_COMMAND_WORLD?(mod_world[0]?mod_world:world_words[lang][min(board.world,EDIT_WORLD_COUNT-1)]):
                    i==EDIT_COMMAND_PREVIOUS?"<":i==EDIT_COMMAND_NEXT?">":labels[7+i];
  if (arrow && hovered==EDIT_TOOL_COUNT+i) rectangle(d,action.x-2,action.y-2,action.w+4,action.h+4,0xff8bdbff);
  image(d,arrow?ART_EDITOR_ARROW: hovered==EDIT_TOOL_COUNT+i?ART_EDITOR_BUTTON_HOVER:ART_EDITOR_BUTTON,
        action.x,action.y,action.w,action.h);
  text(d,label,action.x+action.w*.5f,action.y+4,i==EDIT_COMMAND_COPY?.52f:.69f);
 }
 if (editor_notice) text(d,labels[editor_notice>0?12:13],243,444,.72f);
 else if (editor_tool==EDIT_ZOOG || editor_tool==EDIT_POWER)
  text(d,editor_tool==EDIT_ZOOG?monster_words[lang][editor_monster]:power_words[lang][editor_power],243,444,.73f);
 else if (editor_tool==EDIT_CAGE && editor_color==EDIT_RAINBOW) {
  const char *rainbow[]={"Rainbow cage","Gaiola arco-\xedris","Jaula arco\xedris"};
  text(d,rainbow[lang],243,444,.73f);
 }
 if (editor_picker) draw_editor_picker(d,lang);
 if (editor_confirm) draw_confirm(d,labels[14],library_words[lang][4],library_words[lang][9],editor_confirm-1);
}

static void draw_diamond_effects(IDirect3DDevice9 *d,const IggleDiamondView *view)
{
 if (!load_art(d,ART_DIAMOND) || !load_art(d,ART_AURA)) return;
 float sx=g_ddraw.render.viewport.width/(float)max(1,g_ddraw.width);
 float sy=g_ddraw.render.viewport.height/(float)max(1,g_ddraw.height);
 float ox=(float)g_ddraw.render.viewport.x,oy=(float)g_ddraw.render.viewport.y;
 BOOL sparkle=load_art(d,ART_CRYSTAL_SPARKLE);
 if (view->grace_seconds>0 && view->burst_seconds==0 &&
     ((int)(view->grace_seconds*10.f)&1)) {
  quad(d,art[ART_AURA].tex,ox+(view->hero_x-29)*sx,oy+(view->hero_y-32)*sy,
       58*sx,58*sy,0x73c3ecff,0,0,1,1);
 }
 if (view->shield || view->burst_seconds>0) {
  float burst=view->burst_seconds/.42f;
  float size=view->shield ? 72.f+4.f*sinf(view->orbit*3.f) : 74.f+63.f*(1.f-burst);
  unsigned alpha=view->shield ? (view->slow_seconds>0 ? 220 : 145) : (unsigned)(245.f*burst);
  quad(d,art[ART_AURA].tex,ox+(view->hero_x-size/2)*sx,oy+(view->hero_y-size/2-3)*sy,
       size*sx,size*sy,(alpha<<24)|0xffffff,0,0,1,1);
  for (unsigned i=0;i<3;++i) {
   float angle=view->orbit+i*2.0943951f;
   float depth=sinf(angle);
   float distance=view->shield ? 1.f : 1.f+1.6f*(1.f-burst);
   float x=view->hero_x+32.f*distance*cosf(angle);
   float y=view->hero_y-6.f+19.f*distance*depth;
   float gem_size=19.f+3.f*depth;
   unsigned gem_alpha=view->shield ? (unsigned)(242.f+12.f*depth) : (unsigned)(255.f*burst);
   quad(d,art[ART_DIAMOND].tex,ox+(x-gem_size/2)*sx,oy+(y-gem_size/2)*sy,
        gem_size*sx,gem_size*sy,(gem_alpha<<24)|0xffffff,0,0,1,1);
   float glint=view->shield ? powf(.5f+.5f*sinf(angle*3.f+i*1.7f),9.f) : burst;
   if (sparkle && glint>.03f) {
    float star_size=8.f+9.f*glint;
    unsigned star_alpha=(unsigned)(245.f*glint);
    quad(d,art[ART_CRYSTAL_SPARKLE].tex,ox+(x-gem_size*.13f-star_size/2)*sx,
         oy+(y-gem_size*.17f-star_size/2)*sy,star_size*sx,star_size*sy,
         (star_alpha<<24)|0xffffff,0,0,1,1);
   }
  }
 }
 for (unsigned i=0;i<view->count;++i) {
  const IggleDiamond *gem=&view->gems[i];
  float bob=2.f*sinf(gem->age*4.f);
  unsigned alpha=gem->age>12 && ((int)(gem->age*8)&1)?105:255;
  quad(d,art[ART_DIAMOND].tex,ox+(gem->x-18)*sx,oy+(gem->y-17+bob)*sy,
       36*sx,34*sy,(alpha<<24)|0xffffff,0,0,1,1);
 }
 for (unsigned i=0;i<3;++i) {
  unsigned alpha=i<view->collected?255:70;
  quad(d,art[ART_DIAMOND].tex,ox+(g_ddraw.width-107+i*22)*sx,oy+438*sy,17*sx,17*sy,(alpha<<24)|0xffffff,0,0,1,1);
 }
}

enum { MOD_PREV=5, MOD_NEXT, MOD_PLAY, MOD_BACK };
static const struct Box mod_actions[]={{147,330,48,32},{445,330,48,32},{208,330,135,32},{351,330,83,32}};
static struct Box mod_row(int i) { return (struct Box){151,112+i*37,338,30}; }
static int mod_rows(void) { return max(0,min(5,mod_count-mod_page*5)); }

static void draw_mods(IDirect3DDevice9 *d)
{
 int lang=iggle_current_language();
 const char *const *words=(const char *const [3][8]){
  {"World","Hero","On","Off","Play world","Back","Place .igglemod files in the mods folder.","Could not load this mod."},
  {"Mundo","Her\xf3i","Ativo","Inativo","Jogar mundo","Voltar","Coloque arquivos .igglemod na pasta mods.","Falha ao carregar o mod."},
  {"Mundo","H\xe9roe","Activo","Inactivo","Jugar mundo","Volver","Pon archivos .igglemod en la carpeta mods.","No se pudo cargar el mod."}
 }[lang];
 text(d,"Mods",320,75,1.18f);
 for(int i=0;i<mod_rows();++i) {
  IggleModInfo info;
  if(!iggle_mods_info(mod_page*5+i,&info)) continue;
  char label[94];
  const char *name=info.name;
  if(info.kind==IGGLE_MOD_WORLD && !strcmp(info.id,"sugar-grove"))
   name=(const char *const[]){"Sugar Grove","Bosque de A\xe7\xfa" "car","Bosque de Az\xfa" "car"}[lang];
  snprintf(label,sizeof label,"%s  /  %s  /  %s",words[info.kind==IGGLE_MOD_HERO],name,words[info.enabled?2:3]);
  menu_button(d,mod_row(i),label,hovered==i,FALSE);
 }
 if(mod_count>5) {
  menu_button(d,mod_actions[0],"<",hovered==MOD_PREV,FALSE);
  menu_button(d,mod_actions[1],">",hovered==MOD_NEXT,FALSE);
 }
 if(iggle_mods_world(NULL,NULL)) menu_button(d,mod_actions[2],words[4],hovered==MOD_PLAY,FALSE);
 menu_button(d,mod_actions[3],words[5],hovered==MOD_BACK,TRUE);
 text(d,words[mod_notice?7:6],320,382,.58f);
}

void iggle_overlay_draw(IDirect3DDevice9*d){
 LONG page=InterlockedCompareExchange(&opened,0,0);
 IggleDiamondView view;
 if (!page && !iggle_diamond_view(&view)) return;
 if (page) for(int i=0;i<6;i++)if(!load_art(d,i))return;
 IDirect3DStateBlock9*state=NULL;if(FAILED(IDirect3DDevice9_CreateStateBlock(d,D3DSBT_ALL,&state)))return;
 IDirect3DStateBlock9_Capture(state);
 IDirect3DDevice9_SetPixelShader(d,NULL);IDirect3DDevice9_SetVertexShader(d,NULL);
 IDirect3DDevice9_SetFVF(d,D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);
 IDirect3DDevice9_SetRenderState(d,D3DRS_ALPHABLENDENABLE,TRUE);
 IDirect3DDevice9_SetRenderState(d,D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);IDirect3DDevice9_SetRenderState(d,D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
 IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,FALSE);IDirect3DDevice9_SetRenderState(d,D3DRS_ALPHATESTENABLE,FALSE);
 IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLOROP,D3DTOP_MODULATE);IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLORARG1,D3DTA_TEXTURE);IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
 IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_ALPHAOP,D3DTOP_MODULATE);IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
 IDirect3DDevice9_SetTextureStageState(d,1,D3DTSS_COLOROP,D3DTOP_DISABLE);
 IDirect3DDevice9_SetSamplerState(d,0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);IDirect3DDevice9_SetSamplerState(d,0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
 if (!page) {
  draw_diamond_effects(d,&view);
  IDirect3DStateBlock9_Apply(state);IDirect3DStateBlock9_Release(state);return;
 }
 quad(d,NULL,0,0,g_ddraw.render.width,g_ddraw.render.height,0xaa000000,0,0,1,1);
 if(opened==5) {
  for(int i=ART_HERO;i<=ART_EDITOR_ARROW;++i) if(!load_art(d,i)) {
   IDirect3DStateBlock9_Apply(state);IDirect3DStateBlock9_Release(state);return;
  }
  draw_level_editor(d);
  IDirect3DStateBlock9_Apply(state);IDirect3DStateBlock9_Release(state);return;
 }
 image(d,0,125,57,389,366);
 if(opened==6){
  draw_mods(d);
  IDirect3DStateBlock9_Apply(state);IDirect3DStateBlock9_Release(state);return;
 }
 if(opened==4){
  draw_custom_levels(d);
  IDirect3DStateBlock9_Apply(state);IDirect3DStateBlock9_Release(state);return;
 }
 if(opened==3){
  const int lang=iggle_current_language();
  const char *lines[3][7]={
   {"WASD / Arrows: Move","Left stick / D-pad: Move","A / Right trigger: Boost","Start: Pause   B: Back","D-pad: Select a button","A: Confirm   Right stick: Cursor","Editor: Y switches board / tools"},
   {"WASD / Setas: Mover","Analogico esq. / Direcional: Mover","A / Gatilho dir.: Turbo","Start: Pausar   B: Voltar","Direcional: Selecionar botao","A: Confirmar   Analogico dir.: Cursor","Editor: Y alterna tabuleiro / ferramentas"},
   {"WASD / Flechas: Mover","Palanca izq. / Cruceta: Mover","A / Gatillo der.: Turbo","Start: Pausa   B: Volver","Cruceta: Seleccionar un boton","A: Confirmar   Palanca derecha: Cursor","Editor: Y alterna tablero / herramientas"}};
  text(d,lang==0?"Controls":lang==1?"Controles":"Controles",320,76,1.15f);
  for(int i=0;i<7;i++)text(d,lines[lang][i],320,133+i*27,i==6?.59f:.72f);
  image(d,hovered==4?5:4,269,352,101,43);
  text(d,lang==0?"Done":lang==1?"OK":"Aceptar",320,360,.85f);
  IDirect3DStateBlock9_Apply(state);IDirect3DStateBlock9_Release(state);return;
 }
 if (opened == 2) {
  const int lang = iggle_current_language();
  const char *labels[] = {"English", "Portugu\xeas (Brasil)", "Espa\xf1ol"};
  text(d, lang == 0 ? "Language" : "Idioma", 320, 76, 1.15f);
  int over = InterlockedCompareExchange(&hovered,0,0);
  for (int i=0;i<3;i++) {
   image(d,over==i?2:1,204,135+i*51,231,43);
   text(d,labels[i],320,143+i*51,.85f);
   if(lang==i)text(d,">",190,143+i*51,.85f);
  }
  const int feedback = InterlockedCompareExchange(&language_feedback,0,0);
  if(feedback>0){
   const char *messages[] = {"Reopen the game to apply.", "Reabra o jogo para aplicar.", "Reabre el juego para aplicar."};
   text(d,messages[feedback-1],320,304,.78f);
  } else if(feedback<0) {
   text(d,lang==0?"Could not change the language.":lang==1?"Falha ao trocar o idioma.":"No se pudo cambiar el idioma.",320,304,.68f);
  }
  image(d,over==4?5:4,269,352,101,43);
  text(d,lang==0?"Done":lang==1?"OK":"Aceptar",320,360,.85f);
  IDirect3DStateBlock9_Apply(state);IDirect3DStateBlock9_Release(state);
  return;
 }
 const int lang=iggle_current_language();
 text(d,lang==0?"Display Options":lang==1?"Op\xe7\xf5" "es de v\xED" "deo":"Opciones de video",320,76,1.15f);
 char status[80];snprintf(status,sizeof(status),lang==0?"Output: %d x %d":lang==1?"Resolu\xe7\xe3o: %d x %d":"Resolucion: %d x %d",g_ddraw.render.width,g_ddraw.render.height);text(d,status,320,108,.65f);
 const char*labels[]={"960 x 720","1280 x 720","1600 x 900","1920 x 1080",lang==0?"Native / Borderless":lang==1?"Nativa / Sem bordas":"Nativa / Sin bordes"};
 const int widths[]={960,1280,1600,1920},heights[]={720,720,900,1080};
 int over=InterlockedCompareExchange(&hovered,0,0);
 BOOL matched=FALSE;
 for(int i=0;i<5;i++){
  BOOL active=i<4?g_ddraw.render.width==widths[i] && g_ddraw.render.height==heights[i]:g_config.fullscreen&&!matched;
  if(active)matched=TRUE;
  image(d,over==i?2:1,204,DISPLAY_FIRST_Y+i*DISPLAY_ROW_STEP,231,DISPLAY_ROW_HEIGHT);
  text(d,labels[i],320,DISPLAY_FIRST_Y+5+i*DISPLAY_ROW_STEP,.76f);
  if(active)text(d,">",190,DISPLAY_FIRST_Y+5+i*DISPLAY_ROW_STEP,.76f);
 }
 const char *fsr_on[] = {"FSR 1 - ON", "FSR 1 - Ligado", "FSR 1 - Activado"};
 const char *fsr_off[] = {"FSR 1 - OFF", "FSR 1 - Desligado", "FSR 1 - Desactivado"};
 int fsr_y = DISPLAY_FIRST_Y + DISPLAY_FSR_ROW * DISPLAY_ROW_STEP;
 image(d,over==DISPLAY_FSR_ROW?2:1,204,fsr_y,231,DISPLAY_ROW_HEIGHT);
 text(d,g_config.d3d9_filter==4?fsr_on[lang]:fsr_off[lang],320,fsr_y+5,.76f);
 image(d,over==DISPLAY_DONE_ROW?5:4,269,DISPLAY_DONE_Y,101,43);
 text(d,lang==0?"Done":lang==1?"OK":"Aceptar",320,DISPLAY_DONE_Y+8,.9f);
 IDirect3DStateBlock9_Apply(state);IDirect3DStateBlock9_Release(state);
}
static int done_row(void){return opened==1?DISPLAY_DONE_ROW:4;}
static int option_count(void){return opened==1?DISPLAY_DONE_ROW:opened==2?3:0;}
static int hit(int x,int y){
 float sx=(x-ui_x())/ui_scale(),sy=(y-ui_y())/ui_scale();
 for(int i=0;i<option_count();i++){
  int top=opened==1?DISPLAY_FIRST_Y+i*DISPLAY_ROW_STEP:135+i*51,height=opened==1?DISPLAY_ROW_HEIGHT:43;
  if(sx>=204&&sx<435&&sy>=top&&sy<top+height)return i;
 }
 int done_y = opened==1?DISPLAY_DONE_Y:352;
 if(sx>=269&&sx<370&&sy>=done_y&&sy<done_y+43)return done_row();
 return -1;
}
static void finish_overlay(HWND hwnd,int selected){
 if(opened==2 && selected>=1 && selected<=3){
  InterlockedExchange(&language_feedback,iggle_select_language(hwnd,selected)?selected:-1);
  ReleaseSemaphore(g_ddraw.render.sem,1,NULL);
  return;
 }
 choice=selected;InterlockedExchange(&opened,0);suppress_until=GetTickCount()+250;
 if(selected)PostMessageA(hwnd,WM_APP+0x482,selected,0);
 else {mouse_lock();ReleaseSemaphore(g_ddraw.render.sem,1,NULL);}
}
static void refresh_library(void)
{
 custom_total=iggle_custom_refresh();
 custom_page=0;
 library_selection=custom_total?0:-1;
 library_confirm=0;
 custom_error=FALSE;
 hovered=custom_total?0:CUSTOM_NEW;
}

static void editor_back(void)
{
 iggle_editor_close();
 refresh_library();
 InterlockedExchange(&opened,4);
}

static int select_box(const struct Box *boxes, const int *rows,int count,int selection,int direction)
{
 POINT centers[32];
 int selected=-1;
 for(int i=0;i<count;++i) {
  centers[i]=(POINT){boxes[i].x+boxes[i].w/2,boxes[i].y+boxes[i].h/2};
  if(rows[i]==selection) selected=i;
 }
 int next=iggle_focus_next(centers,count,selected,direction);
 return next>=0?rows[next]:selection;
}

static int focus_direction(WPARAM key)
{
 switch(key) {
 case VK_UP:return 0;
 case VK_DOWN:return 1;
 case VK_LEFT:return 2;
 case VK_RIGHT:return 3;
 default:return -1;
 }
}

static BOOL library_button_available(int row)
{
 if (row==CUSTOM_PLAY || row==CUSTOM_EDIT || row==CUSTOM_DELETE) return custom_visible()>0;
 if (row==CUSTOM_PREV || row==CUSTOM_NEXT) return custom_total>IGGLE_LEVEL_PAGE_SIZE;
 return TRUE;
}

static void move_library_focus(int direction)
{
 struct Box boxes[IGGLE_LEVEL_PAGE_SIZE+ARRAYSIZE(library_actions)];
 int rows[ARRAYSIZE(boxes)],count=0;
 for(int i=0;i<custom_visible();++i) {
  rows[count]=i;
  boxes[count++]=(struct Box){153,121+36*i,334,29};
 }
 for(int i=CUSTOM_PLAY;i<=CUSTOM_NEXT;++i) if(library_button_available(i)) {
  rows[count]=i;
  boxes[count++]=library_actions[i-CUSTOM_PLAY];
 }
 hovered=select_box(boxes,rows,count,hovered,direction);
 if (hovered>=0 && hovered<custom_visible()) library_selection=hovered;
}

static void editor_open(int index)
{
 trace_editor("open editor",index,0);
 wchar_t path[MAX_PATH];
 if (index>=0 && !iggle_custom_path_at(index,path)) { custom_error=TRUE; return; }
 if (!iggle_editor_open(index>=0?path:NULL)) { custom_error=TRUE; return; }
 editor_copy=index<0 || !iggle_custom_in_game(index);
 editor_path[0]=0;
 if (!editor_copy) wcscpy(editor_path,path);
 editor_panel_focus=FALSE;
 editor_panel_item=0;
 editor_tool=EDIT_WALL;
 editor_picker=picker_focus=editor_monster=editor_power=0;
 editor_color=editor_x=editor_y=editor_side=editor_notice=editor_confirm=0;
 hovered=-1;
 InterlockedExchange(&opened,5);
}

static void delete_selected(void)
{
 wchar_t path[MAX_PATH];
 int index=custom_page*IGGLE_LEVEL_PAGE_SIZE+library_selection;
 if (library_selection<0 || !iggle_custom_path_at(index,path) || !DeleteFileW(path)) {
  library_confirm=0;
  custom_error=TRUE;
  return;
 }
 refresh_library();
}

static void custom_activate(HWND hwnd, int row)
{
 if (library_confirm) {
  if (library_confirm==1) delete_selected();
  else library_confirm=0;
  return;
 }
 custom_error=FALSE;
 if (row==CUSTOM_BACK) finish_overlay(hwnd,0);
 else if (row==CUSTOM_NEW) editor_open(-1);
 else if (row==CUSTOM_PREV || row==CUSTOM_NEXT) {
  int last=max(0,(custom_total-1)/IGGLE_LEVEL_PAGE_SIZE);
  custom_page=max(0,min(last,custom_page+(row==CUSTOM_NEXT?1:-1)));
  hovered=library_selection=custom_visible()?0:-1;
 } else if (row>=0 && row<custom_visible()) {
  library_selection=row;
  hovered=CUSTOM_PLAY;
 } else if (library_selection>=0) {
  int index=custom_page*IGGLE_LEVEL_PAGE_SIZE+library_selection;
  if (row==CUSTOM_DELETE) library_confirm=2;
  if (row==CUSTOM_EDIT) editor_open(index);
  if (row==CUSTOM_PLAY) {
   if (iggle_custom_select(index) && iggle_custom_launch(custom_menu)) finish_overlay(hwnd,0);
   else custom_error=TRUE;
  }
 }
}

static int custom_mouse(float x,float y)
{
 if (library_confirm) {
  if (inside(confirm_yes,x,y)) return 0;
  if (inside(confirm_no,x,y)) return 1;
  return -1;
 }
 for (int i=0;i<custom_visible();++i)
  if (inside((struct Box){153,121+36*i,334,29},x,y)) return i;
 for (int i=CUSTOM_PLAY;i<=CUSTOM_NEXT;++i) {
  if (!custom_total && (i==CUSTOM_PLAY || i==CUSTOM_EDIT || i==CUSTOM_DELETE)) continue;
  if (i>=CUSTOM_PREV && custom_total<=IGGLE_LEVEL_PAGE_SIZE) continue;
  if (inside(library_actions[i-CUSTOM_PLAY],x,y)) return i;
 }
 return -1;
}

static BOOL custom_message(HWND hwnd,UINT msg,WPARAM key,LPARAM point)
{
 if (msg==WM_KEYDOWN) {
  int rows=custom_visible();
  if (library_confirm) {
   if (key==VK_ESCAPE) library_confirm=0;
   else if (key==VK_LEFT || key==VK_RIGHT) library_confirm=3-library_confirm;
   else if (key==VK_RETURN) custom_activate(hwnd,0);
  } else if (key==VK_ESCAPE) custom_activate(hwnd,CUSTOM_BACK);
  else if (key==VK_PRIOR) custom_activate(hwnd,CUSTOM_PREV);
  else if (key==VK_NEXT) custom_activate(hwnd,CUSTOM_NEXT);
  else if (focus_direction(key)>=0) move_library_focus(focus_direction(key));
  else if (key==VK_RETURN) custom_activate(hwnd,hovered<0?(rows?CUSTOM_PLAY:CUSTOM_NEW):hovered);
  else if (key==VK_DELETE) custom_activate(hwnd,CUSTOM_DELETE);
 } else if (msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST) {
  float x=(GET_X_LPARAM(point)-ui_x())/ui_scale(),y=(GET_Y_LPARAM(point)-ui_y())/ui_scale();
  int row=custom_mouse(x,y);
  if (library_confirm) {
   if (row>=0) {
    library_confirm=row+1;
    if (msg==WM_LBUTTONUP) custom_activate(hwnd,row);
   }
  } else {
   hovered=row;
   if (msg==WM_LBUTTONUP && row>=0) custom_activate(hwnd,row);
   if (msg==WM_LBUTTONDBLCLK && row>=0 && row<custom_visible()) {
    library_selection=row;
    custom_activate(hwnd,CUSTOM_PLAY);
   }
  }
 } else if (msg<WM_KEYFIRST || msg>WM_KEYLAST) return FALSE;
 ReleaseSemaphore(g_ddraw.render.sem,1,NULL);
 return TRUE;
}

static BOOL editor_save(BOOL copy)
{
 wchar_t destination[MAX_PATH];
 if (copy || editor_copy) {
  if (!iggle_custom_new_path(destination)) { trace_editor("no new path",GetLastError(),0); editor_notice=-1; return FALSE; }
 } else wcscpy(destination,editor_path);
 BOOL saved=iggle_editor_save(destination);
 trace_editor("save editor",saved,copy);
 editor_notice=saved?1:-1;
 if (saved) {
  wcscpy(editor_path,destination);
  editor_copy=FALSE;
 }
 return saved;
}

static void editor_choose_tool(int tool)
{
 editor_tool=tool;
 if(tool!=EDIT_CAGE && editor_color==EDIT_RAINBOW) editor_color=0;
 editor_notice=0;
 if (tool==EDIT_ZOOG || tool==EDIT_POWER) {
  editor_picker=tool;
  picker_focus=tool==EDIT_ZOOG?editor_monster:editor_power;
 }
}

static void pick_editor_variant(int choice)
{
 int count=picker_count();
 if (choice<0 || choice>count) return;
 if (choice<count) {
  if (editor_picker==EDIT_ZOOG) editor_monster=choice;
  else editor_power=choice;
 }
 editor_picker=0;
}

static void editor_brush(BOOL erase)
{
 int variant=editor_tool==EDIT_ZOOG?editor_monster:editor_power;
 editor_notice=0;
 iggle_editor_paint(editor_x,editor_y,editor_side,erase?EDIT_ERASE:editor_tool,editor_color==EDIT_RAINBOW && (erase || editor_tool!=EDIT_CAGE)?0:editor_color,variant);
}

static BOOL editor_picker_message(UINT msg,WPARAM key,LPARAM point)
{
 int count=picker_count();
 if(msg==WM_KEYDOWN) {
  if(key==VK_ESCAPE) editor_picker=0;
  else if(key==VK_RETURN) pick_editor_variant(picker_focus);
  else if(key==VK_UP) picker_focus=picker_focus==count?count-1:max(0,picker_focus-3);
  else if(key==VK_DOWN) picker_focus=min(count,picker_focus+3);
  else if(key==VK_LEFT || key==VK_RIGHT)
   picker_focus=max(0,min(count,picker_focus+(key==VK_RIGHT?1:-1)));
 } else if(msg==WM_MOUSEMOVE || msg==WM_LBUTTONUP) {
  float x=(GET_X_LPARAM(point)-ui_x())/ui_scale(),y=(GET_Y_LPARAM(point)-ui_y())/ui_scale();
  int index=-1;
  for(int i=0;i<count;++i) if(inside(picker_box(i),x,y)) index=i;
  if(inside((struct Box){265,355,110,37},x,y)) index=count;
  if(index>=0) {
   picker_focus=index;
   if(msg==WM_LBUTTONUP) pick_editor_variant(index);
  }
 }
 ReleaseSemaphore(g_ddraw.render.sem,1,NULL);
 return TRUE;
}

static void editor_activate(HWND hwnd,int index)
{
 if (index<EDIT_TOOL_COUNT) { editor_choose_tool(index); return; }
 switch (index-EDIT_TOOL_COUNT) {
 case EDIT_COMMAND_SAVE: editor_save(FALSE); break;
 case EDIT_COMMAND_COPY: editor_save(TRUE); break;
 case EDIT_COMMAND_COLOR: editor_color=(editor_color+1)%(EDIT_COLOR_COUNT+(editor_tool==EDIT_CAGE)); break;
 case EDIT_COMMAND_PREVIOUS:
 case EDIT_COMMAND_NEXT: {
  int step=index-EDIT_TOOL_COUNT==EDIT_COMMAND_NEXT?1:-1;
  if(editor_tool==EDIT_ZOOG) editor_monster=(editor_monster+step+EDIT_ZOOG_COUNT)%EDIT_ZOOG_COUNT;
  else if(editor_tool==EDIT_POWER) editor_power=(editor_power+step+EDIT_POWER_COUNT)%EDIT_POWER_COUNT;
  else if(editor_tool==EDIT_CAGE || editor_tool==EDIT_HOME) {
   int count=EDIT_COLOR_COUNT+(editor_tool==EDIT_CAGE);
   editor_color=(editor_color+step+count)%count;
  }
  break;
 }
 case EDIT_COMMAND_WORLD: {
  EditorBoard board;
  iggle_editor_board(&board);
  int worlds=EDIT_WORLD_COUNT+iggle_mods_world(NULL,NULL);
  if (iggle_editor_set_world((board.world+1)%worlds)) editor_notice=0;
  break;
 }
 case EDIT_COMMAND_BACK:
  if (iggle_editor_dirty()) editor_confirm=2;
  else editor_back();
  break;
 case EDIT_COMMAND_TEST:
  if ((editor_copy || iggle_editor_dirty()) && !editor_save(FALSE)) break;
  if (iggle_custom_select_path(editor_path) && iggle_custom_launch(custom_menu)) {
   iggle_editor_close();
   finish_overlay(hwnd,0);
  } else editor_notice=-1;
  break;
 }
}

static BOOL editor_variant_arrows(void)
{
 return editor_tool==EDIT_ZOOG || editor_tool==EDIT_POWER || editor_tool==EDIT_CAGE || editor_tool==EDIT_HOME;
}

static void move_editor_panel(int direction)
{
 struct Box boxes[EDIT_TOOL_COUNT+ARRAYSIZE(editor_actions)+EDIT_COLOR_COUNT+1];
 int rows[ARRAYSIZE(boxes)],count=0;
 for(int i=0;i<EDIT_TOOL_COUNT;++i) {
  rows[count]=i;
  boxes[count++]=editor_tool_box(i);
 }
 for(int i=0;i<ARRAYSIZE(editor_actions);++i) {
  if (i==EDIT_COMMAND_COLOR || (i>=EDIT_COMMAND_PREVIOUS && !editor_variant_arrows())) continue;
  rows[count]=EDIT_TOOL_COUNT+i;
  boxes[count++]=editor_actions[i];
 }
 for(int i=0;i<EDIT_COLOR_COUNT+(editor_tool==EDIT_CAGE);++i) {
  rows[count]=EDIT_TOOL_COUNT+ARRAYSIZE(editor_actions)+i;
  boxes[count++]=(struct Box){489+i*25,323,20,17};
 }
 editor_panel_item=select_box(boxes,rows,count,editor_panel_item,direction);
 hovered=editor_panel_item;
}

static BOOL editor_message(HWND hwnd,UINT msg,WPARAM key,LPARAM point)
{
 if ((msg<WM_KEYFIRST || msg>WM_KEYLAST) && (msg<WM_MOUSEFIRST || msg>WM_MOUSELAST)) return FALSE;
 if(editor_picker) return editor_picker_message(msg,key,point);
 EditorBoard board;
 iggle_editor_board(&board);
 if (msg==WM_KEYDOWN) {
  if (editor_confirm) {
   if (key==VK_ESCAPE) editor_confirm=0;
   else if (key==VK_LEFT || key==VK_RIGHT) editor_confirm=3-editor_confirm;
   else if (key==VK_RETURN) {
    if (editor_confirm==1) editor_back();
    editor_confirm=0;
   }
  } else if (key==VK_ESCAPE && editor_panel_focus) {
   editor_panel_focus=FALSE;
   hovered=-1;
  } else if (key==VK_ESCAPE) editor_activate(hwnd,EDIT_TOOL_COUNT+EDIT_COMMAND_BACK);
  else if (key=='T') {
   editor_panel_focus=!editor_panel_focus;
   if (editor_panel_focus) hovered=editor_panel_item=editor_tool;
   else hovered=-1;
  } else if (editor_panel_focus && focus_direction(key)>=0) move_editor_panel(focus_direction(key));
  else if (editor_panel_focus && key==VK_RETURN) {
   int first_color=EDIT_TOOL_COUNT+ARRAYSIZE(editor_actions);
   if (editor_panel_item>=first_color) editor_color=editor_panel_item-first_color;
   else editor_activate(hwnd,editor_panel_item);
  }
  else if (key==VK_TAB || key==VK_PRIOR || key==VK_NEXT) {
   int direction=key==VK_PRIOR || (key==VK_TAB && (GetKeyState(VK_SHIFT)&0x8000))?-1:1;
   editor_tool=(editor_tool+EDIT_TOOL_COUNT+direction)%EDIT_TOOL_COUNT;
   if(editor_tool!=EDIT_CAGE && editor_color==EDIT_RAINBOW) editor_color=0;
  } else if (key>='1' && key<='8') editor_choose_tool((int)(key-'1'));
  else if (key=='M') editor_activate(hwnd,EDIT_TOOL_COUNT+EDIT_COMMAND_WORLD);
  else if (key=='C') editor_activate(hwnd,EDIT_TOOL_COUNT+EDIT_COMMAND_COLOR);
  else if (key=='S' || key==VK_F2) editor_save(FALSE);
  else if (key=='P' || key==VK_F5) editor_activate(hwnd,EDIT_TOOL_COUNT+EDIT_COMMAND_TEST);
  else if (key==VK_RETURN || key==VK_DELETE) {
   editor_brush(key==VK_DELETE);
  } else if (key==VK_UP || key==VK_LEFT || key==VK_DOWN || key==VK_RIGHT) {
   editor_side=key==VK_UP?0:key==VK_LEFT?1:key==VK_DOWN?2:3;
   editor_x=max(0,min(board.columns-1,editor_x+(key==VK_RIGHT)-(key==VK_LEFT)));
   editor_y=max(0,min(board.rows-1,editor_y+(key==VK_DOWN)-(key==VK_UP)));
  }
 } else if (msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST) {
  float x=(GET_X_LPARAM(point)-ui_x())/ui_scale(),y=(GET_Y_LPARAM(point)-ui_y())/ui_scale();
  if (editor_confirm) {
   if (inside(confirm_yes,x,y)) { editor_confirm=1; if (msg==WM_LBUTTONUP) editor_back(); }
   else if (inside(confirm_no,x,y)) { editor_confirm=2; if (msg==WM_LBUTTONUP) editor_confirm=0; }
  } else {
   if (msg==WM_LBUTTONUP || msg==WM_RBUTTONUP) editor_panel_focus=FALSE;
   if (msg==WM_MOUSEMOVE && editor_panel_focus) return TRUE;
   hovered=-1;
   for (int i=0;i<EDIT_TOOL_COUNT;++i)
    if (inside(editor_tool_box(i),x,y)) hovered=i;
   for (int i=0;i<ARRAYSIZE(editor_actions);++i)
    if (inside(editor_actions[i],x,y)) hovered=EDIT_TOOL_COUNT+i;
   if (msg==WM_LBUTTONUP) trace_editor("editor click",(int)x,(int)y);
   if (hovered>=0 && msg==WM_LBUTTONUP) {
    if(hovered==EDIT_TOOL_COUNT+EDIT_COMMAND_COLOR) {
     int color=(int)(x-489)/25;
     if(x>=489 && x<489+25*(EDIT_COLOR_COUNT+(editor_tool==EDIT_CAGE))) editor_color=color;
    } else editor_activate(hwnd,hovered);
   }
   float left,top,step=editor_geometry(&board,&left,&top);
   if (step>0 && x>=left && y>=top && x<left+step*board.columns && y<top+step*board.rows) {
    editor_x=(int)((x-left)/step);editor_y=(int)((y-top)/step);
    float dx=(x-left)/step-editor_x-.5f, dy=(y-top)/step-editor_y-.5f;
    editor_side=dx*dx>dy*dy?(dx>0?3:1):(dy>0?2:0);
    if (msg==WM_LBUTTONUP || msg==WM_RBUTTONUP) {
     editor_brush(msg==WM_RBUTTONUP);
    }
   }
  }
 }
 ReleaseSemaphore(g_ddraw.render.sem,1,NULL);
 return TRUE;
}

static BOOL mod_button(int row)
{
 if (row<MOD_PREV) return row>=0 && row<mod_rows();
 if (row<=MOD_NEXT) return mod_count>5;
 return row==MOD_BACK || (row==MOD_PLAY && iggle_mods_world(NULL,NULL));
}

static void mods_activate(HWND hwnd,int row)
{
 if (!mod_button(row)) return;
 if (row==MOD_BACK) finish_overlay(hwnd,0);
 else if (row==MOD_PREV || row==MOD_NEXT) {
  mod_page=max(0,min((mod_count-1)/5,mod_page+(row==MOD_NEXT?1:-1)));
  hovered=0;
 } else if (row==MOD_PLAY) {
  wchar_t path[MAX_PATH];
  if (iggle_mods_stage(path) && iggle_custom_select_path(path) && iggle_custom_launch(custom_menu))
   finish_overlay(hwnd,0);
  else mod_notice=1;
 } else {
  mod_notice=!iggle_mods_toggle(mod_page*5+row);
  trace_editor("mod toggled",mod_page*5+row,mod_notice);
 }
}

static BOOL mods_message(HWND hwnd,UINT msg,WPARAM key,LPARAM point)
{
 if(msg==WM_KEYDOWN) {
  if(key==VK_ESCAPE) mods_activate(hwnd,MOD_BACK);
  else if(key==VK_RETURN) mods_activate(hwnd,hovered);
  else if(focus_direction(key)>=0) {
   struct Box boxes[9];
   int rows[9],count=0;
   for(int i=0;i<=MOD_BACK;++i) if(mod_button(i)) {
    rows[count]=i;
    boxes[count++]=i<MOD_PREV?mod_row(i):mod_actions[i-MOD_PREV];
   }
   hovered=select_box(boxes,rows,count,hovered,focus_direction(key));
  }
 } else if (msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST) {
  float x=(GET_X_LPARAM(point)-ui_x())/ui_scale(),y=(GET_Y_LPARAM(point)-ui_y())/ui_scale();
  hovered=-1;
  for(int i=0;i<=MOD_BACK;++i) if(mod_button(i) && inside(i<MOD_PREV?mod_row(i):mod_actions[i-MOD_PREV],x,y)) hovered=i;
  if(msg==WM_LBUTTONUP) mods_activate(hwnd,hovered);
 } else return msg>=WM_KEYFIRST && msg<=WM_KEYLAST;
 ReleaseSemaphore(g_ddraw.render.sem,1,NULL);
 return TRUE;
}

BOOL iggle_overlay_message(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
 if(!InterlockedCompareExchange(&opened,0,0)){
  if((LONG)(GetTickCount()-suppress_until)<0 && ((msg>=WM_KEYFIRST&&msg<=WM_KEYLAST)||(msg>=WM_MOUSEFIRST&&msg<=WM_MOUSELAST)))return TRUE;
  return FALSE;
 }
 if(opened==6 && mods_message(hwnd,msg,wp,lp))return TRUE;
 if(opened==5 && editor_message(hwnd,msg,wp,lp))return TRUE;
 if(opened==4 && custom_message(hwnd,msg,wp,lp))return TRUE;
 if(msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN){
  if(wp==VK_ESCAPE||wp==VK_F10){finish_overlay(hwnd,0);}
  else if(opened==1 && wp==VK_F6){toggle_fsr();}
  else if(wp==VK_DOWN||wp==VK_UP){
   int last=done_row(),count=option_count();
   if(!count)hovered=last;
   else if(wp==VK_DOWN)hovered=hovered<0||hovered>=last?0:hovered==count-1?last:hovered+1;
   else hovered=hovered<=0||hovered>last?last:hovered==last?count-1:hovered-1;
  }
  else if(wp==VK_RETURN){
   if(opened==1 && hovered==DISPLAY_FSR_ROW)toggle_fsr();
   else finish_overlay(hwnd,hovered>=0&&hovered<option_count()?hovered+1:0);
  }
  ReleaseSemaphore(g_ddraw.render.sem,1,NULL);return TRUE;
 }
 if(msg>=WM_KEYFIRST&&msg<=WM_KEYLAST)return TRUE;
 if(msg>=WM_MOUSEFIRST&&msg<=WM_MOUSELAST){
  int row=hit(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));InterlockedExchange(&hovered,row);
  if(msg==WM_LBUTTONUP&&row>=0){
   if(opened==1&&row==DISPLAY_FSR_ROW){toggle_fsr();}
   else finish_overlay(hwnd,row<option_count()?row+1:0);
  }
  ReleaseSemaphore(g_ddraw.render.sem,1,NULL);return TRUE;
 }
 if(msg==WM_CLOSE)InterlockedExchange(&opened,0);
 return FALSE;
}
static void open_overlay(int page){
 if(opened)return;
 char path[MAX_PATH];snprintf(path,sizeof(path),"%siggle-hd\\menuwindowlarge.rgba",g_config.game_path);
 if(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES)return;
 choice=0;InterlockedExchange(&hovered,-1);InterlockedExchange(&opened,page);
 mouse_unlock();ReleaseSemaphore(g_ddraw.render.sem,1,NULL);
 /* Modeless: the native Options screen already pauses gameplay; keep audio streaming. */
}
void iggle_display_menu(HWND hwnd){ open_overlay(1); }
void iggle_open_language(void){
 InterlockedExchange(&language_feedback,0);
 open_overlay(2);
}
void iggle_apply_display(HWND hwnd,int selected){
 if(selected>=1&&selected<=5){
  MONITORINFO mi={sizeof(mi)};GetMonitorInfo(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&mi);
  int nativeW=mi.rcMonitor.right-mi.rcMonitor.left,nativeH=mi.rcMonitor.bottom-mi.rcMonitor.top;
  const int widths[]={960,1280,1600,1920},heights[]={720,720,900,1080};
  int w=selected==5?nativeW:widths[selected-1],h=selected==5?nativeH:heights[selected-1];
  g_config.windowed=TRUE;g_config.nonexclusive=TRUE;
  g_config.fullscreen=selected==5||(w==nativeW&&h==nativeH);
  if(!g_config.fullscreen){
   RECT frame={0,0,0,0};AdjustWindowRectEx(&frame,real_GetWindowLongA(hwnd,GWL_STYLE),FALSE,real_GetWindowLongA(hwnd,GWL_EXSTYLE));
   double fit=min(1.0,min((double)(mi.rcWork.right-mi.rcWork.left-frame.right+frame.left)/w,(double)(mi.rcWork.bottom-mi.rcWork.top-frame.bottom+frame.top)/h));
   w=(int)(w*fit);h=(int)(h*fit);
  }
  util_set_window_rect(-32000,-32000,w,h,0);
  g_config.upscaled_state=g_config.fullscreen;
  char value[24];snprintf(value,sizeof value,"%d",w);WritePrivateProfileStringA("ddraw","width",value,g_config.ini_path);
  snprintf(value,sizeof value,"%d",h);WritePrivateProfileStringA("ddraw","height",value,g_config.ini_path);
  WritePrivateProfileStringA("ddraw","fullscreen",g_config.fullscreen?"true":"false",g_config.ini_path);
  WritePrivateProfileStringA("ddraw","windowed","true",g_config.ini_path);
  WritePrivateProfileStringA("ddraw","posX","-32000",g_config.ini_path);WritePrivateProfileStringA("ddraw","posY","-32000",g_config.ini_path);
 }
 suppress_until=GetTickCount()+250;mouse_lock();ReleaseSemaphore(g_ddraw.render.sem,1,NULL);
}

void iggle_open_controls(void){open_overlay(3);}

void iggle_open_custom_levels(void *menu)
{
 if (opened) return;
 custom_menu = menu;
 open_overlay(4);
 if (opened == 4) refresh_library();
 trace_editor("open library",opened,custom_total);
}

void iggle_open_mods(void *menu)
{
 if (opened) return;
 mod_count=iggle_mods_scan();
 mod_page=mod_notice=0;
 custom_menu=menu;
 open_overlay(6);
 hovered=mod_count?0:MOD_BACK;
 ReleaseSemaphore(g_ddraw.render.sem,1,NULL);
}
