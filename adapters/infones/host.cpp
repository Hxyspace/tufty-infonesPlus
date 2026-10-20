#include "core_api/host.hpp"
#include "adapters/infones/cartridge.hpp"
#include "InfoNES.h"
#include "InfoNES_System.h"
#include <cstdarg>
#include <cstdio>

namespace {
const nes::Host *active=nullptr;
WORD linebuffer[256];
bool running=false;
void message(const char *fmt,va_list args){
  char text[192];std::vsnprintf(text,sizeof text,fmt,args);
  if(active && active->message)active->message(text);
}
}
WORD NesPalette[64]={
  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
  16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
  32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
  48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
};
int InfoNES_Menu(){return 1;}
int InfoNES_ReadRom(const char *){return -1;}
void InfoNES_ReleaseRom(){ROM=VROM=nullptr;}
void InfoNES_MessageBox(const char *fmt,...){va_list a;va_start(a,fmt);message(fmt,a);va_end(a);}
void InfoNES_Error(const char *fmt,...){va_list a;va_start(a,fmt);message(fmt,a);va_end(a);}
void InfoNES_DebugPrint(const char *s){if(active && active->message)active->message(s);}
void InfoNES_PadState(DWORD *p1,DWORD *p2,DWORD *system){
  std::uint8_t a=0,b=0;bool quit=false;
  if(active && active->input)active->input(a,b,quit);else quit=true;
  *p1=a;*p2=b;*system=quit?PAD_SYS_QUIT:0;
}
int InfoNES_LoadFrame(){if(active && active->frame)active->frame();return 0;}
void InfoNES_PreDrawLine(int){InfoNES_SetLineBuffer(linebuffer,256);}
void InfoNES_PostDrawLine(int y){
  if(active && active->line)active->line(y,linebuffer,PPU_R1,InfoNES_IsPal());
}
void InfoNES_SoundInit(){}
int InfoNES_SoundOpen(int,int){return 0;}
void InfoNES_SoundClose(){}
bool InfoNES_HasSoundOutput(){return false;}
int InfoNES_GetSoundBufferSize(){return 0;}
void InfoNES_SoundOutput(int,BYTE *,BYTE *,BYTE *,BYTE *,BYTE *,BYTE *){}
namespace infones::port {
void service(){if(active && active->service)active->service();}
}
namespace nes::infones_adapter {
bool run(const Host &host,const std::uint8_t *rom,std::size_t bytes,const char **error){
  if(running){if(error)*error="Emulator is already running";return false;}
  running=true;active=&host;
  bool ok=load(rom,bytes,error);
  if(ok){
    InfoNES_Cycle();ok=!infones::memory::failed;
    if(!ok && error)*error="Cartridge runtime allocation failed";
    unload();
  }
  active=nullptr;running=false;return ok;
}
}
