#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_Mapper.h"
#include "InfoNES_pAPU.h"
#include "K6502.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Synthetic cartridges and actual CPU/PPU/APU/mapper code, no device stubs.
static std::array<WORD,256*240> frame{};
WORD NesPalette[64];
unsigned int rom_crc=0;
static unsigned frames=0, services=0, samples=0;
static std::size_t allocations=0;
static std::vector<BYTE> prg(32768),chr(8192);
namespace infones::port {
void *allocate(std::size_t n) {auto p=std::malloc(n); if(p)++allocations; return p;}
void release(void *p) {if(p){assert(allocations);--allocations;}std::free(p);}
bool has_large_memory(){return true;}
bool remove_sprite_limit(){return false;}
void service(){++services;}
}
int InfoNES_Menu(){return 0;}
int InfoNES_ReadRom(const char *){return -1;}
void InfoNES_ReleaseRom(){ROM=VROM=nullptr;}
int InfoNES_LoadFrame(){return ++frames>=4 ? -1:0;}
void InfoNES_PadState(DWORD *a,DWORD *b,DWORD *s){*a=0x49;*b=0;*s=frames>=4?PAD_SYS_QUIT:0;}
void InfoNES_DebugPrint(const char *){}
void InfoNES_MessageBox(const char *,...){}
void InfoNES_Error(const char *,...){}
void InfoNES_SoundInit(){}
int InfoNES_SoundOpen(int,int){return 0;}
void InfoNES_SoundClose(){}
int InfoNES_GetSoundBufferSize(){return APU_MAX_SAMPLES_PER_SYNC;}
void InfoNES_SoundOutput(int n,BYTE *a,BYTE *,BYTE *,BYTE *,BYTE *,BYTE *){assert(n>=0&&n<=APU_MAX_SAMPLES_PER_SYNC);samples+=n; if(n)assert(a);}
void InfoNES_PreDrawLine(int y){assert(y>=0&&y<240);InfoNES_SetLineBuffer(frame.data()+y*256,256);}
void InfoNES_PostDrawLine(int){}

static void image(unsigned mapper) {
  std::memset(&NesHeader,0,sizeof NesHeader);
  NesHeader.byRomSize=2; NesHeader.byVRomSize=1;
  NesHeader.byInfo1=(mapper&15)<<4; NesHeader.byInfo2=mapper&0xf0;
  std::fill(prg.begin(),prg.end(),0xea); std::fill(chr.begin(),chr.end(),0);
  // SEI; CLD; LDX #$ff; TXS; LDA #$23; STA $02; INC $02; JMP loop.
  const BYTE code[]={0x78,0xd8,0xa2,0xff,0x9a,0xa9,0x23,0x85,0x02,0xe6,0x02,0x4c,0x09,0x80};
  std::copy(std::begin(code),std::end(code),prg.begin());
  for(auto offset:{0x7ffa,0x7ffc,0x7ffe}){prg[offset]=0;prg[offset+1]=0x80;}
  ROM=prg.data();VROM=chr.data();rom_crc=0;
}
static void nrom() {
  image(0); InfoNES_SetRegion(INFONES_REGION_NTSC); InfoNES_Init();
  assert(InfoNES_Reset()==0);
  K6502_Step(80); assert(RAM[2]!=0x23 && RAM[2]!=0);
  for(unsigned i=0;i<64;++i)NesPalette[i]=i;
  std::fill(chr.begin(),chr.begin()+8,255); InfoNES_SetupChr();
  PPU_Scanline=16; PPU_R1=R1_SHOW_SCR|R1_CLIP_BG;
  PPU_Addr=PPU_Temp=0; PPU_Scr_H_Bit=PPU_Scr_H_Byte=0;
  PPU_NameTableBank=NAME_TABLE0; PalTable[0]=0x8000;PalTable[1]=0x21;
  InfoNES_PreDrawLine(16);InfoNES_DrawLine();
  for(unsigned x=0;x<256;++x)assert(frame[16*256+x]==0x21);
  frames=services=samples=0;InfoNES_Cycle();
  assert(frames==4 && samples>0 && services>500);
  unsigned hash=2166136261u;
  for(auto byte:frame){hash=(hash^byte)*16777619u;}
  for(unsigned i=0;i<RAM_SIZE;++i){hash=(hash^RAM[i])*16777619u;}
  assert(hash==0x34f09e54u && samples==2844);
  std::printf("NROM digest=%08x audio_samples=%u frames=%u\n",hash,samples,frames);
  InfoNES_Fin();assert(allocations==0);
}
static void mapper48() {
  image(48);InfoNES_Init();assert(InfoNES_Reset()==0);
  for(unsigned v=0;v<256;++v)for(unsigned slot=0;slot<4;++slot){
    MapperWrite(0xa000+slot,v);
    // Plus already fixes the old pico-nes 1 KiB bank bug.
    assert(PPUBANK[4+slot]==VROM+(v%8)*1024);
  }
  InfoNES_Fin();assert(allocations==0);
}
int main(){nrom();mapper48();puts("core cartridge regression passed");}
