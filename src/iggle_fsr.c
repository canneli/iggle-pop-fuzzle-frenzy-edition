#include <windows.h>
#include <d3d9.h>
#include "config.h"
#include "dd.h"
#include "iggle_fsr_shaders.h"
static IDirect3DPixelShader9 *easu,*rcas;
static IDirect3DTexture9 *intermediate;
static IDirect3DSurface9 *target;
static int width,height;
static BOOL unavailable;
struct FsrVertex {float x,y,z,rhw,u,v;};
void iggle_fsr_release(void){
 if(target)IDirect3DSurface9_Release(target);
 if(intermediate)IDirect3DTexture9_Release(intermediate);
 if(easu)IDirect3DPixelShader9_Release(easu);
 if(rcas)IDirect3DPixelShader9_Release(rcas);
 target=NULL;intermediate=NULL;easu=NULL;rcas=NULL;width=height=0;unavailable=FALSE;
}
static HRESULT draw_quad(IDirect3DDevice9 *device,float x,float y,float w,float h,float u,float v){
 struct FsrVertex vertices[]={{x-.5f,y-.5f,0,1,0,0},{x+w-.5f,y-.5f,0,1,u,0},{x-.5f,y+h-.5f,0,1,0,v},{x+w-.5f,y+h-.5f,0,1,u,v}};
 return IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLESTRIP,2,vertices,sizeof vertices[0]);
}
BOOL iggle_fsr_draw(IDirect3DDevice9 *device,int tex_width,int tex_height){
 if(unavailable || g_config.d3d9_filter!=4 || g_ddraw.bpp==8)return FALSE;
 int w=g_ddraw.render.viewport.width,h=g_ddraw.render.viewport.height;
 if(w<g_ddraw.width || h<g_ddraw.height)return FALSE;
 if(!easu && (FAILED(IDirect3DDevice9_CreatePixelShader(device,IGGLE_FSR_EASU,&easu)) || FAILED(IDirect3DDevice9_CreatePixelShader(device,IGGLE_FSR_RCAS,&rcas)))){iggle_fsr_release();unavailable=TRUE;return FALSE;}
 if(w!=width || h!=height){
  if(target){IDirect3DSurface9_Release(target);target=NULL;}
  if(intermediate){IDirect3DTexture9_Release(intermediate);intermediate=NULL;}
  if(FAILED(IDirect3DDevice9_CreateTexture(device,w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&intermediate,NULL)) || FAILED(IDirect3DTexture9_GetSurfaceLevel(intermediate,0,&target))){iggle_fsr_release();unavailable=TRUE;return FALSE;}
  width=w;height=h;
 }
 IDirect3DStateBlock9 *state=NULL;IDirect3DSurface9 *backbuffer=NULL;IDirect3DBaseTexture9 *source=NULL;
 if(FAILED(IDirect3DDevice9_CreateStateBlock(device,D3DSBT_ALL,&state)))return FALSE;
 IDirect3DStateBlock9_Capture(state);
 HRESULT result=IDirect3DDevice9_GetRenderTarget(device,0,&backbuffer);
 if(SUCCEEDED(result))result=IDirect3DDevice9_GetTexture(device,0,&source);
 D3DVIEWPORT9 viewport;IDirect3DDevice9_GetViewport(device,&viewport);
 if(SUCCEEDED(result) && source){
  IDirect3DDevice9_SetTexture(device,0,NULL);
  result=IDirect3DDevice9_SetRenderTarget(device,0,target);
  D3DVIEWPORT9 stage={0,0,w,h,0,1};IDirect3DDevice9_SetViewport(device,&stage);
  IDirect3DDevice9_SetFVF(device,D3DFVF_XYZRHW|D3DFVF_TEX1);
  IDirect3DDevice9_SetVertexShader(device,NULL);
  IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE);
  IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHABLENDENABLE,FALSE);
  IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHATESTENABLE,FALSE);
  IDirect3DDevice9_SetRenderState(device,D3DRS_SCISSORTESTENABLE,FALSE);
  IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_MINFILTER,D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
  IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  float constants[12]={(float)tex_width,(float)tex_height,0,0,(float)g_ddraw.width,(float)g_ddraw.height,0,0,(float)w,(float)h,0,0};
  IDirect3DDevice9_SetPixelShaderConstantF(device,0,constants,3);
  IDirect3DDevice9_SetTexture(device,0,source);
  IDirect3DDevice9_SetPixelShader(device,easu);
  if(SUCCEEDED(result))result=draw_quad(device,0,0,w,h,(float)g_ddraw.width/tex_width,(float)g_ddraw.height/tex_height);
  IDirect3DDevice9_SetTexture(device,0,NULL);
  if(SUCCEEDED(result))result=IDirect3DDevice9_SetRenderTarget(device,0,backbuffer);
  IDirect3DDevice9_SetViewport(device,&viewport);
  IDirect3DDevice9_SetTexture(device,0,(IDirect3DBaseTexture9*)intermediate);
  IDirect3DDevice9_SetPixelShader(device,rcas);
  if(SUCCEEDED(result))result=draw_quad(device,g_ddraw.render.viewport.x,g_ddraw.render.viewport.y,w,h,1,1);
 } else result=E_FAIL;
 IDirect3DDevice9_SetTexture(device,0,NULL);
 if(backbuffer)IDirect3DDevice9_SetRenderTarget(device,0,backbuffer);
 IDirect3DDevice9_SetViewport(device,&viewport);
 IDirect3DStateBlock9_Apply(state);IDirect3DStateBlock9_Release(state);
 if(backbuffer)IDirect3DSurface9_Release(backbuffer);
 if(source)IDirect3DBaseTexture9_Release(source);
 return SUCCEEDED(result);
}
