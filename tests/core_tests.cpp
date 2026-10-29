#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_Mapper.h"
#include "InfoNES_pAPU.h"
#include "K6502.h"
#include "adapters/infones/cartridge.hpp"
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
static int fail_at=-1, attempts=0;
static int sound_capacity=APU_MAX_SAMPLES_PER_SYNC;
static bool sound_output=true;
static bool check_dendy_scroll=false;
static unsigned restored_scroll_lines=0;
static std::array<unsigned,240> drawn{};
static std::vector<BYTE> prg(32768),chr(8192);
namespace infones::port {
void *allocate(std::size_t n) {
  if (attempts++==fail_at) return nullptr;
  auto p=std::malloc(n); if(p)++allocations; return p;
}
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
bool InfoNES_HasSoundOutput(){return sound_output;}
int InfoNES_GetSoundBufferSize(){return sound_capacity;}
void InfoNES_SoundOutput(int n,BYTE *a,BYTE *,BYTE *,BYTE *,BYTE *,BYTE *){assert(n>=0&&n<=APU_MAX_SAMPLES_PER_SYNC);samples+=n; if(n)assert(a);}
void InfoNES_PreDrawLine(int y){
  assert(y>=0&&y<240);
  if(check_dendy_scroll && y==0 && frames>0){
    // The NMI writes VRAM, then restores a 40-pixel vertical scroll. It must
    // finish before the pre-render line copies t to v for the next frame.
    assert((PPU_Addr&0x73e0)==0x00a0);
    assert((PPU_Addr&0x7be0)==(PPU_Temp&0x7be0));
    assert(RAM[3]==frames);
    ++restored_scroll_lines;
  }
  InfoNES_SetLineBuffer(frame.data()+y*256,256);
}
void InfoNES_PostDrawLine(int y){++drawn[y];}

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
  frames=services=samples=0;drawn.fill(0);InfoNES_Cycle();
  assert(frames==4 && samples>0 && services>500);
  unsigned hash=2166136261u;
  for(auto byte:frame)hash=(hash^byte)*16777619u;
  // The direct line test above starts the first emulated frame at row 16.
  for(unsigned y=0;y<240;++y)assert(drawn[y]==(y<16?3:4));
  for(unsigned i=0;i<RAM_SIZE;++i){hash=(hash^RAM[i])*16777619u;}
  std::printf("NROM digest=%08x audio_samples=%u frames=%u\n",hash,samples,frames);
  std::fflush(stdout);
  // S1's 34f09e54 included a miscompiled absolute JMP and clipped edge rows.
  assert(RAM[2]==0x73 && RAM[0x1fd]==0 && RAM[0x1fe]==0 && RAM[0x1ff]==0);
  assert(hash==0x1844660eu && samples==2844);
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
static void dendy_vblank_scroll() {
  image(0);InfoNES_SetRegion(INFONES_REGION_DENDY);
  // Enable rendering/NMI, then wait. The handler resets the VRAM address and
  // spends about 2300 CPU cycles on an update before restoring $2005.
  const BYTE reset[]={0x78,0xd8,0xa2,0xff,0x9a,
    0xa9,0x0a,0x8d,0x01,0x20,0xa9,0x80,0x8d,0x00,0x20,0x4c,0x0f,0x80};
  const BYTE nmi[]={0x48,0x8a,0x48,0x98,0x48,
    0xa9,0x20,0x8d,0x06,0x20,0xa9,0x00,0x8d,0x06,0x20,
    0xa0,0x02,0xa2,0xe6,0xca,0xd0,0xfd,0x88,0xd0,0xf8,
    0xa9,0x00,0x8d,0x05,0x20,0xa9,0x28,0x8d,0x05,0x20,
    0xe6,0x03,0x68,0xa8,0x68,0xaa,0x68,0x40};
  std::copy(std::begin(reset),std::end(reset),prg.begin());
  std::copy(std::begin(nmi),std::end(nmi),prg.begin()+0x100);
  prg[0x7ffa]=0;prg[0x7ffb]=0x81;
  assert(InfoNES_Init());assert(InfoNES_Reset()==0);
  frames=services=samples=restored_scroll_lines=0;check_dendy_scroll=true;
  InfoNES_Cycle();check_dendy_scroll=false;
  assert(frames==4 && restored_scroll_lines==3);
  InfoNES_Fin();assert(allocations==0);
  InfoNES_SetRegion(INFONES_REGION_NTSC);
}
static void clipping() {
  image(0);assert(InfoNES_Init());assert(InfoNES_Reset()==0);
  std::memset(PPURAM,0,PPURAM_SIZE);std::memset(SPRRAM,255,SPRRAM_SIZE);
  for(int i=0;i<16;++i)PPUBANK[i]=PPURAM+i*1024;
  for(int i=0;i<32;++i)PalTable[i]=(i%4)?i:0x800d;
  PPU_R0=PPU_R1=PPU_R2=0;
  PPU_Addr=PPU_Temp=0;PPU_Scr_H_Byte=PPU_Scr_H_Bit=0;
  PPU_NameTableBank=NAME_TABLE0;PPU_UpDown_Clip=0;PPU_SP_Height=8;PPU_Scanline=16;
  std::fill_n(PPURAM,8,255);
  SPRRAM[SPR_Y]=15;SPRRAM[SPR_CHR]=SPRRAM[SPR_ATTR]=SPRRAM[SPR_X]=0;
  InfoNES_PreDrawLine(16);auto line=frame.data()+16*256;
  for(unsigned mask=0;mask<4;++mask){
    PPU_R1=R1_SHOW_SCR|R1_SHOW_SP|(mask&1?R1_CLIP_BG:0)|(mask&2?R1_CLIP_SP:0);
    InfoNES_DrawLine();
    WORD left=mask&2?PalTable[17]:mask&1?PalTable[1]:PalTable[0];
    for(int x=0;x<8;++x)assert(line[x]==left);
    for(int x=8;x<256;++x)assert(line[x]==PalTable[1]);
  }
  SPRRAM[SPR_ATTR]=SPR_ATTR_PRI;PPU_R1=R1_SHOW_SCR|R1_SHOW_SP|R1_CLIP_SP;
  InfoNES_DrawLine();for(int x=0;x<8;++x)assert(line[x]==PalTable[17]);
  PPU_R1|=R1_CLIP_BG;InfoNES_DrawLine();for(int x=0;x<256;++x)assert(line[x]==PalTable[1]);
  PPU_R1=0;InfoNES_DrawLine();for(int x=0;x<256;++x)assert(line[x]==PalTable[0]);
  InfoNES_Fin();
}
static void cpu_operands() {
  image(0);
  const BYTE program[]={0xa9,0x5a,0x8d,0x01,0x06,0xa2,0x03,0x9d,0x00,0x06,
                        0x20,0x20,0x80,0xad,0x02,0x06,0x85,0x04,0x4c,0x12,0x80};
  const BYTE subroutine[]={0xa9,0xa5,0x8d,0x02,0x06,0x60};
  std::copy(std::begin(program),std::end(program),prg.begin());
  std::copy(std::begin(subroutine),std::end(subroutine),prg.begin()+0x20);
  assert(InfoNES_Init());assert(InfoNES_Reset()==0);K6502_Step(200);
  assert(RAM[0x601]==0x5a && RAM[0x603]==0x5a && RAM[0x602]==0xa5 && RAM[4]==0xa5);
  InfoNES_Fin();
}
static void lifecycle_and_oom() {
  prg.resize(512*1024);chr.resize(512*1024);
  for(bool output:{true,false}){
  sound_output=output;
  for(unsigned mapper:{0,4,5,13,16,19,24,30,69,85,96,111,159,185,188,235,263,268,555}) {
    auto prepare=[&]{
      image(mapper);NesHeader.byRomSize=32;NesHeader.byVRomSize=64;
      if(mapper==4 || mapper==13 || mapper==30 || mapper==85 || mapper==96 || mapper==111 || mapper==268){
        NesHeader.byVRomSize=0;VROM=nullptr;
      }
      NesHeader.byInfo2|=8;NesHeader.byReserve[0]=mapper>>8;
      if(mapper==4 || mapper==268)NesHeader.byReserve[3]=12;
    };
    fail_at=-1;attempts=0;prepare();assert(InfoNES_Init());assert(InfoNES_Reset()==0);
    int points=attempts;auto live=allocations;
    assert(InfoNES_Reset()==0);assert(allocations==live);
    InfoNES_Fin();InfoNES_Fin();assert(allocations==0);
    for(int i=0;i<points;++i){
      fail_at=i;attempts=0;prepare();
      bool initialized=InfoNES_Init();
      assert(!initialized || InfoNES_Reset()<0);
      InfoNES_Fin();InfoNES_Fin();assert(allocations==0);
      assert(!RAM && !SRAM && !PPURAM && !SPRRAM && !ROM && !VROM);
    }
    fail_at=-1;
    std::printf("mapper %u: %d allocation failure points and repeated reset passed\n",mapper,points);
  }
  image(260);NesHeader.byInfo2|=8;NesHeader.byReserve[0]=1;
  assert(InfoNES_Init());assert(InfoNES_Reset()<0);assert(MapperNo==260);
  InfoNES_Fin();assert(allocations==0);
  }
  sound_output=true;
}
static void audio_backpressure() {
  struct Snapshot {std::array<BYTE,RAM_SIZE> ram;int dmc;std::uint32_t cycles;ApuDmcIrq_t irq;};
  const auto run=[](int capacity,bool mute,bool output){
    sound_output=output;
    image(0);assert(InfoNES_Init());assert(InfoNES_Reset()==0);
    // DMC interrupt, fastest rate, 65-byte sample from $C000.
    const BYTE code[]={0xa9,0x8f,0x8d,0x10,0x40,0xa9,0,0x8d,0x12,0x40,
                       0xa9,4,0x8d,0x13,0x40,0xa9,0x10,0x8d,0x15,0x40,
                       0x4c,0x14,0x80};
    std::copy(std::begin(code),std::end(code),prg.begin());
    sound_capacity=capacity;APU_Mute=mute;frames=samples=0;InfoNES_Cycle();
    Snapshot s{};std::copy_n(RAM,RAM_SIZE,s.ram.begin());s.dmc=ApuC5DmaLength;
    s.cycles=K6502_Now();s.irq=ApuDmcIrq;
    if(capacity<=0 || !output)assert(samples==0);else assert(samples>0);
    InfoNES_Fin();return s;
  };
  const auto reference=run(APU_MAX_SAMPLES_PER_SYNC,false,true);
  for(auto capacity:{0,-10,1,APU_MAX_SAMPLES_PER_SYNC})for(bool mute:{false,true})for(bool output:{true,false}){
    const auto s=run(capacity,mute,output);
    assert(s.ram==reference.ram && s.dmc==reference.dmc && s.cycles==reference.cycles);
    assert(std::memcmp(&s.irq,&reference.irq,sizeof s.irq)==0);
  }
  sound_capacity=APU_MAX_SAMPLES_PER_SYNC;APU_Mute=0;sound_output=true;
}
static void cartridge_loading() {
  using namespace nes;
  std::vector<BYTE> rom(16+512+32768+8192);
  const auto valid=[&]{
    std::fill(rom.begin(),rom.end(),0);std::memcpy(rom.data(),"NES\x1a",4);
    rom[4]=2;rom[5]=1;rom[6]=4;
    std::fill(rom.begin()+16,rom.begin()+528,0x5a);
    rom[528]=0x4c;rom[529]=0;rom[530]=0x80;
    for(auto offset:{0x7ffa,0x7ffc,0x7ffe}){rom[528+offset]=0;rom[528+offset+1]=0x80;}
  };
  valid();Cartridge c;const char *error=nullptr;
  assert(!inspect(nullptr,0,c,&error) && error);
  for(std::size_t n=0;n<rom.size();++n)assert(!inspect(rom.data(),n,c));
  assert(inspect(rom.data(),rom.size(),c));assert(c.prg_offset==528 && c.mapper==0);
  assert(infones_adapter::load(rom.data(),rom.size(),&error) && !error);
  assert(ROM==rom.data()+528 && SRAM[0x1000]==0x5a);
  auto old=ROM;rom[7]=4;
  assert(!infones_adapter::load(rom.data(),rom.size(),&error) && error && ROM==old);
  valid();rom[7]=8;assert(inspect(rom.data(),rom.size(),c) && c.nes2);
  assert(infones_adapter::load(rom.data(),rom.size(),&error));
  // Resolve legal multi-region headers to the same timing used by the host
  // frame clock. Loading after PAL/Dendy must not inherit the previous region.
  for(unsigned timing:{1,2,3,1,3,2,0}) {
    rom[12]=timing;
    const auto original=rom;
    const auto expected=timing==2?0:timing;
    assert(inspect(rom.data(),rom.size(),c) && c.nes2 && c.timing==expected);
    assert(c.header[12]==timing);
    assert(infones_adapter::load(rom.data(),rom.size(),&error) && !error);
    assert(InfoNES_GetRegion()==(expected==1?INFONES_REGION_PAL:
           expected==3?INFONES_REGION_DENDY:INFONES_REGION_NTSC));
    assert(SCANLINE_FRAC_NUM==(expected==3?1:0));
    assert(SCANLINE_FRAC_DEN==(expected==3?3:1));
    if(expected==3)assert(STEP_PER_SCANLINE==114 && STEP_PER_FRAME==35464);
    assert(NesHeader.byReserve[4]==timing && rom==original);
  }
  for(unsigned timing:{4,6,0xfe}) {
    rom[12]=timing;auto previous=ROM;
    assert(!infones_adapter::load(rom.data(),rom.size(),&error) && error && ROM==previous);
  }
  rom[12]=0;
  rom[8]=1;assert(inspect(rom.data(),rom.size(),c) && c.mapper==256);
  assert(!infones_adapter::load(rom.data(),rom.size(),&error));
  rom[8]=0x10;assert(!infones_adapter::load(rom.data(),rom.size(),&error));
  rom[8]=0;rom[9]=0x0f;assert(!inspect(rom.data(),rom.size(),c));
  valid();rom[12]=1;assert(!inspect(rom.data(),rom.size(),c));
  valid();rom[7]=8;rom[10]=8;assert(!infones_adapter::load(rom.data(),rom.size(),&error));
  infones_adapter::unload();infones_adapter::unload();assert(allocations==0);
  for(int point=0;point<5;++point){
    valid();fail_at=point;attempts=0;
    assert(!infones_adapter::load(rom.data(),rom.size(),&error) && error && allocations==0);
  }
  fail_at=-1;
  valid();assert(infones_adapter::load(rom.data(),rom.size(),&error));
  rom[7]=8;assert(infones_adapter::load(rom.data(),rom.size(),&error));
  rom[7]=0;assert(infones_adapter::load(rom.data(),rom.size(),&error));
  infones_adapter::unload();assert(allocations==0);
}
static void admitted_bank_bounds() {
  for(auto mapper:nes::admitted_mappers)for(bool large:{false,true}){
    unsigned prg_banks=large && mapper!=0 && mapper!=3?32:2;
    if(large){
      if(mapper==11 || mapper==70)prg_banks=8;
      if(mapper==8 || mapper==206)prg_banks=8;
      if(mapper==18 || mapper==23 || mapper==24 || mapper==25 || mapper==71)prg_banks=16;
      if(mapper==15 || mapper==177 || mapper==241)prg_banks=64;
    }
    unsigned chr_banks=large && mapper!=0?mapper==3?4:32:1;
    if(mapper==2 || mapper==7 || mapper==15 || mapper==71 || mapper==177 || mapper==241)chr_banks=0;
    if(large && (mapper==11 || mapper==70))chr_banks=16;
    if(large && mapper==8)chr_banks=4;
    if(large && mapper==206)chr_banks=8;
    switch(mapper){
      case 22:case 80:case 207:prg_banks=large?16:2;chr_banks=large?16:1;break;
      case 32:prg_banks=large?16:2;break;
      case 34:case 226:prg_banks=large?64:2;chr_banks=0;break;
      case 140:prg_banks=large?8:2;chr_banks=large?16:1;break;
      case 41:case 113:prg_banks=large?16:2;chr_banks=large?16:1;break;
      case 72:case 78:case 89:case 76:case 88:prg_banks=large?8:2;chr_banks=large?16:1;break;
      case 75:case 92:prg_banks=large?16:2;chr_banks=large?16:1;break;
      case 79:prg_banks=large?4:2;chr_banks=large?8:1;break;
      case 133:prg_banks=large?4:2;chr_banks=large?4:1;break;
      case 86:case 244:prg_banks=large?8:2;chr_banks=large?8:1;break;
      case 87:case 184:prg_banks=2;chr_banks=large?4:1;break;
      case 97:prg_banks=large?16:2;chr_banks=0;break;
      case 240:chr_banks=large?16:1;break;
      case 242:chr_banks=0;break;
      case 243:prg_banks=large?8:2;chr_banks=large?16:1;break;
    }
    std::vector<BYTE> rom(16+prg_banks*16384+chr_banks*8192);
    std::memcpy(rom.data(),"NES\x1a",4);rom[4]=prg_banks;rom[5]=chr_banks;
    rom[6]=(mapper&15)<<4;rom[7]=mapper&0xf0;
    if(mapper==78){rom[7]|=8;rom[8]=0x30;}
    if(mapper==210){rom[7]|=8;rom[8]=0x20;}
    if(mapper==242)rom[6]|=2;
    if(mapper>255){rom[7]|=8;rom[8]=mapper>>8;}
    const char *error=nullptr;
    assert(nes::infones_adapter::load(rom.data(),rom.size(),&error));
    const auto inside=[](const BYTE *p,const BYTE *base,std::size_t bytes,std::size_t window){
      const auto a=reinterpret_cast<std::uintptr_t>(p),b=reinterpret_cast<std::uintptr_t>(base);
      return base && a>=b && a-b<=bytes && window<=bytes-(a-b);
    };
    const auto check=[&]{
      for(auto p:ROMBANK)assert(inside(p,ROM,prg_banks*16384,8192));
      for(unsigned slot=0;slot<8;++slot)
        assert(inside(PPUBANK[slot],VROM,chr_banks*8192,1024) || inside(PPUBANK[slot],PPURAM,PPURAM_SIZE,1024));
    };
    check();
    for(unsigned value=0;value<256;++value)for(WORD address:{0x8000,0x8001,0xa000,0xa001,0xc000,0xc001,0xe000,0xe001}){
      MapperWrite(address,value);check();
    }
    for(unsigned value=0;value<256;++value){
      for(WORD address:{0x6000,0x6100,0x67ff,0x6800,0x7100,0x7101,0x7fff}){
        MapperSram(address,value);check();
      }
      for(WORD address:{0x4020,0x4100,0x4101,0x4120,0x4900,0x5fff}){
        MapperApu(address,value);check();
      }
    }
    nes::infones_adapter::unload();assert(allocations==0);
  }
}
static std::vector<BYTE> banked_cartridge(unsigned mapper,unsigned chr_banks,bool nes2=false) {
  constexpr unsigned prg_bytes=128*1024;
  std::vector<BYTE> rom(16+prg_bytes+chr_banks*8192);
  std::memcpy(rom.data(),"NES\x1a",4);rom[4]=8;rom[5]=chr_banks;
  rom[6]=(mapper&15)<<4;rom[7]=(mapper&0xf0)|(nes2?8:0);
  if(nes2)rom[8]=mapper>>8;
  for(unsigned n=0;n<prg_bytes/8192;++n)std::fill_n(rom.data()+16+n*8192,8192,n);
  for(unsigned n=0;n<chr_banks*8;++n)std::fill_n(rom.data()+16+prg_bytes+n*1024,1024,n);
  return rom;
}
static void simple_mapper_boards() {
  const auto load=[](std::vector<BYTE> &rom){
    const char *error=nullptr;
    assert(nes::infones_adapter::load(rom.data(),rom.size(),&error) && !error);
  };
  const auto finish=[]{nes::infones_adapter::unload();assert(allocations==0);};
  const auto prg_bank=[](unsigned slot,unsigned bank){assert(ROMBANK[slot]==ROM+bank*8192);};
  const auto chr_bank=[](unsigned slot,unsigned bank){assert(PPUBANK[slot]==VROM+bank*1024);};
  const auto mirror=[](bool horizontal){
    assert(PPUBANK[8]==PPUBANK[horizontal?9:10]);
    assert(PPUBANK[8]!=PPUBANK[horizontal?10:9]);
  };
  // Bus conflicts must sample the old CPU window, not the newly selected bank.
  for(unsigned mapper:{2,3})for(unsigned sub:{0,2}){
    auto rom=banked_cartridge(mapper,mapper==3?4:0,true);
    rom[8]=sub<<4;if(mapper==2)rom[11]=7;
    // CNROM has fixed 32 KiB PRG.
    if(mapper==3){rom.erase(rom.begin()+16+32768,rom.begin()+16+128*1024);rom[4]=2;}
    const auto original=rom;load(rom);
    for(unsigned value=0;value<256;++value)for(WORD address:{0x8123,0xa456,0xc789,0xfabc}){
      const unsigned latched=value & (sub==2?ROMBANK[(address-0x8000)/8192][address&8191]:255);
      MapperWrite(address,value);
      if(mapper==2){prg_bank(0,(latched%8)*2);prg_bank(1,(latched%8)*2+1);prg_bank(2,14);prg_bank(3,15);}
      else for(unsigned slot=0;slot<8;++slot)chr_bank(slot,(latched%4)*8+slot);
    }
    assert(rom==original);finish();
  }
  for(unsigned chr_banks:{0,16}){
    auto rom=banked_cartridge(11,chr_banks);rom[6]|=1;
    const auto original=rom;load(rom);mirror(false);
    for(unsigned value=0;value<256;++value)for(WORD address:{0x8123,0xa456,0xc789,0xfabc}){
      // Legacy mapper 11 covers boards with and without bus conflicts.
      // Retain permissive writes (e.g. Free Fall), unlike explicit 2.2/3.2.
      const unsigned latched=value;
      MapperWrite(address,value);
      for(unsigned slot=0;slot<4;++slot)prg_bank(slot,(latched&3)*4+slot);
      if(chr_banks)for(unsigned slot=0;slot<8;++slot)chr_bank(slot,(latched>>4)*8+slot);
      else assert(PPUBANK[0]==PPURAM);
      mirror(false);
    }
    assert(rom==original);finish();
  }
  auto rom=banked_cartridge(33,32);load(rom);
  for(unsigned value=0;value<256;++value){
    // $c000/$c001 are mirrors of $8000/$8001, not IRQ registers.
    MapperWrite(0xdffc,value);prg_bank(0,(value&63)%16);mirror(value&64);
    MapperWrite(0x9ffd,value);prg_bank(1,(value&63)%16);
    for(unsigned pair=0;pair<2;++pair){
      MapperWrite(0xc002+pair,value);
      chr_bank(pair*2,(value*2)%256);chr_bank(pair*2+1,(value*2+1)%256);
    }
    for(unsigned slot=0;slot<4;++slot){MapperWrite(0xfffc+slot,value);chr_bank(4+slot,value);}
  }
  for(unsigned n=0;n<300;++n)MapperHSync();assert(IRQ_State==IRQ_Wiring);finish();
  rom=banked_cartridge(70,16);load(rom);mirror(false);
  for(unsigned value=0;value<256;++value){
    MapperWrite(0xbfff,value);
    prg_bank(0,((value>>4)&7)*2);prg_bank(1,((value>>4)&7)*2+1);
    prg_bank(2,14);prg_bank(3,15);
    for(unsigned slot=0;slot<8;++slot)chr_bank(slot,(value&15)*8+slot);
  }
  assert(PPUBANK[8]==PPUBANK[11] && PPUBANK[8]==PPURAM+0x2400);
  MapperWrite(0x8000,0);assert(PPUBANK[8]==PPURAM+0x2000);
  assert(InfoNES_Reset()==0);mirror(false);finish();
  rom=banked_cartridge(71,0);load(rom);
  MapperWrite(0xc123,3);prg_bank(0,6);
  MapperWrite(0x8123,0);prg_bank(0,6);
  MapperWrite(0x9000,0x10);assert(PPUBANK[8]==PPURAM+0x2400);prg_bank(0,6);
  MapperWrite(0x9fff,0);assert(PPUBANK[8]==PPURAM+0x2000);prg_bank(0,6);
  for(unsigned value=0;value<256;++value){MapperWrite(0xffff,value);prg_bank(0,(value%8)*2);prg_bank(2,14);}
  assert(InfoNES_Reset()==0);prg_bank(0,0);assert(PPUBANK[8]!=PPUBANK[11]);finish();
  // Mapper 241 can address 1 MiB. Every address in the register window latches.
  rom=banked_cartridge(241,0);rom.resize(16+1024*1024);rom[4]=64;load(rom);
  for(unsigned value=0;value<256;++value)for(WORD address:{0x8000,0x9123,0xbfff,0xffff}){
    MapperWrite(address,value);
    for(unsigned slot=0;slot<4;++slot)prg_bank(slot,(value%32)*4+slot);
  }
  finish();
  // Admission failures must preserve the running cartridge.
  rom=banked_cartridge(0,1);rom[4]=2;rom.erase(rom.begin()+16+32768,rom.begin()+16+128*1024);load(rom);
  const auto previous=ROM;
  for(unsigned mapper:{2,3,11,33,70,71,241}){
    auto bad=banked_cartridge(mapper,1,true);bad[8]=0xf0;
    assert(!nes::infones_adapter::load(bad.data(),bad.size()) && ROM==previous);
    bad[8]=0;bad[10]=8;
    assert(!nes::infones_adapter::load(bad.data(),bad.size()) && ROM==previous);
  }
  for(unsigned mapper:{11,70,71}){
    auto bad=banked_cartridge(mapper,mapper==71?0:16);
    bad.resize(16+512*1024+bad[5]*8192);bad[4]=32;
    assert(!nes::infones_adapter::load(bad.data(),bad.size()) && ROM==previous);
  }
  finish();
}
static void hengge177() {
  for(unsigned bytes:{32768,128*1024,512*1024,1024*1024})
    for(bool nes2:{false,true})for(bool battery:{false,true}) {
      std::vector<BYTE> rom(16+bytes);
      std::memcpy(rom.data(),"NES\x1a",4);rom[4]=bytes/16384;
      rom[6]=0x10|(battery?2:0);rom[7]=0xb0|(nes2?8:0);
      if(nes2){rom[10]=battery?0x70:7;rom[11]=7;rom[12]=3;}
      for(unsigned n=0;n<bytes/8192;++n)std::fill_n(rom.data()+16+n*8192,8192,n);
      const auto original=rom;
      assert(nes::infones_adapter::load(rom.data(),rom.size()));
      for(unsigned pass=0;pass<2;++pass) {
        for(unsigned i=0;i<4;++i)assert(ROMBANK[i]==ROM+i*8192);
        assert(PPUBANK[8]==PPUBANK[10] && PPUBANK[9]==PPUBANK[11]);
        assert(!MapperReadSram && !MapperSramOwnsWrite);
        PPURAM[17]=0x5a;
        for(unsigned data=0;data<256;++data)for(WORD address:{0x8000,0x9123,0xbfff,0xffff}) {
          MapperWrite(address,data);
          const unsigned page=((data&31)%(bytes/32768))*4;
          for(unsigned i=0;i<4;++i)assert(ROMBANK[i]==ROM+(page+i)*8192);
          assert(PPUBANK[8]==PPUBANK[data&32?9:10]);
          assert(PPUBANK[8]!=PPUBANK[data&32?10:9]);
          assert(PPUBANK[0]==PPURAM && PPURAM[17]==0x5a && byVramWriteEnable);
        }
        // Low-address SRAM writes are plain RAM, not bank registers. Use the
        // real CPU bus to check both ends of WRAM and the absence of conflicts.
        const auto prg0=ROMBANK0;
        const BYTE code[]={0xa9,0xa5,0x8d,7,0x60,0x8d,0xff,0x7f,
                           0xad,7,0x60,0x85,4,0xad,0xff,0x7f,0x85,5,0x4c,18,2};
        std::copy(std::begin(code),std::end(code),RAM+0x200);
        SRAMwritten=false;PC=0x200;K6502_Step(60);
        assert(SRAMwritten && SRAM[7]==0xa5 && SRAM[8191]==0xa5);
        assert(RAM[4]==0xa5 && RAM[5]==0xa5 && ROMBANK0==prg0);
        for(unsigned n=0;n<400;++n)MapperHSync();
        assert(IRQ_State==IRQ_Wiring && rom==original);
        assert(InfoNES_Reset()==0);
      }
      // Invalid boards must not disturb the loaded cartridge or its mapping.
      const auto active=ROMBANK0;
      for(unsigned kind=0;kind<8;++kind) {
        auto bad=rom;bad[7]|=8;bad[10]=battery?0x70:7;bad[11]=7;
        if(kind==0){bad[5]=1;bad[11]=0;bad.resize(bad.size()+8192);}
        if(kind==1)bad[6]|=8;
        if(kind==2){bad[4]=128;bad.resize(16+2*1024*1024);}
        if(kind==3)bad[8]=0x10;
        if(kind==4)bad[10]=5;
        if(kind==5)bad[10]=8;
        if(kind==6)bad[10]=0;
        if(kind==7)bad[11]=8;
        assert(!nes::infones_adapter::load(bad.data(),bad.size()));
        assert(ROMBANK0==active && MapperNo==177);
      }
      nes::infones_adapter::unload();assert(allocations==0);
    }
}
static void medium_mapper_boards() {
  const auto load=[](std::vector<BYTE> &rom){assert(nes::infones_adapter::load(rom.data(),rom.size()));};
  const auto finish=[]{nes::infones_adapter::unload();assert(allocations==0);};
  const auto prg_bank=[](unsigned slot,unsigned bank){assert(ROMBANK[slot]==ROM+bank*8192);};
  const auto chr_bank=[](unsigned slot,unsigned bank){assert(PPUBANK[slot]==VROM+bank*1024);};
  // K-1029's four PRG modes, including aliases and the upper half of 1 MiB.
  auto rom=banked_cartridge(15,0);rom.resize(16+1024*1024);rom[4]=64;load(rom);
  for(unsigned value=0;value<256;++value)for(unsigned mode=0;mode<4;++mode){
    MapperWrite(0xbffc+mode,value);
    unsigned bank=value&63;
    std::array<unsigned,4> expected{};
    switch(mode){
      case 0:bank&=62;expected={bank*2,bank*2+1,bank*2+2,bank*2+3};break;
      case 1:expected={bank*2,bank*2+1,((bank&56)|7)*2,((bank&56)|7)*2+1};break;
      case 2:bank=bank*2+(value>>7);expected={bank,bank,bank,bank};break;
      case 3:expected={bank*2,bank*2+1,bank*2,bank*2+1};break;
    }
    for(unsigned slot=0;slot<4;++slot)prg_bank(slot,expected[slot]);
    assert(PPUBANK[8]==PPUBANK[value&64?9:10]);
    assert(PPUBANK[0]==PPURAM && byVramWriteEnable);
  }
  finish();
  rom=banked_cartridge(18,32);load(rom);
  // Registers are nibble-wide and mirrored over the rest of the CPU block.
  for(unsigned value=0;value<256;++value){
    for(unsigned slot=0;slot<3;++slot){
      const WORD a=slot<2?0x8ffc+slot*2:0x9ffc;
      MapperWrite(a,value&15);MapperWrite(a+1,value>>4);prg_bank(slot,value%16);
    }
    for(unsigned slot=0;slot<8;++slot){
      const WORD a=0xaffc+(slot/2)*0x1000+(slot%2)*2;
      MapperWrite(a,value&15);MapperWrite(a+1,value>>4);chr_bank(slot,value);
    }
  }
  for(unsigned mirror=0;mirror<4;++mirror){
    MapperWrite(0xfffe,mirror);
    if(mirror<2)assert(PPUBANK[8]==PPUBANK[mirror==0?9:10]);
    else for(unsigned slot=8;slot<12;++slot)assert(PPUBANK[slot]==PPURAM+0x2000+(mirror-2)*1024);
  }
  // Compare the scanline aggregate against a per-clock counter oracle for
  // every width, priority when multiple width bits are set, and all regions.
  extern WORD Map18_IRQ_Cnt;
  for(auto region:{INFONES_REGION_NTSC,INFONES_REGION_PAL,INFONES_REGION_DENDY}){
    InfoNES_SetRegion(region);
    for(unsigned control:{1,3,5,9,15})for(unsigned initial:{0,1,0xabc1,0xffff}){
      assert(InfoNES_Reset()==0);
      for(unsigned nibble=0;nibble<4;++nibble)MapperWrite(0xeffc+nibble,(initial>>(nibble*4))&15);
      MapperWrite(0xfffc,0);assert(Map18_IRQ_Cnt==initial);
      MapperWrite(0xfffd,control);
      const unsigned mask=control&8?15:control&4?255:control&2?4095:65535;
      unsigned reference=initial;
      for(unsigned line=0;line<20;++line){
        bool fired=false;
        for(unsigned clock=0;clock<STEP_PER_SCANLINE;++clock){
          if(!(reference&mask))fired=true;
          const unsigned low=(reference-1)&mask;
          reference=(reference&~mask)|low;
        }
        IRQ_State=IRQ_Wiring;MapperHSync();
        assert(Map18_IRQ_Cnt==reference && (IRQ_State!=IRQ_Wiring)==fired);
      }
    }
  }
  IRQ_State=0;APU_Reg[0x15]|=0x40;MapperWrite(0xf001,0);assert(IRQ_State==0);
  APU_Reg[0x15]&=~0x40;MapperWrite(0xf001,0);assert(IRQ_State==IRQ_Wiring);
  assert(InfoNES_Reset()==0);for(unsigned line=0;line<400;++line)MapperHSync();assert(IRQ_State==IRQ_Wiring);
  finish();InfoNES_SetRegion(INFONES_REGION_NTSC);
  for(unsigned sub:{0,2,3}){
    rom=banked_cartridge(23,32,true);rom[8]=sub<<4;load(rom);
    const unsigned shift=sub==2?2:0;
    for(unsigned value=0;value<256;++value){
      MapperWrite(0x8fff,value);
      for(unsigned mode:{0,2,0}){
        MapperWrite(0x9000+(2<<shift),mode);
        prg_bank(0,mode && sub!=3?14:(value&31)%16);
        prg_bank(2,mode && sub!=3?(value&31)%16:14);
      }
      MapperWrite(0xafff,value);prg_bank(1,(value&31)%16);
      for(unsigned slot=0;slot<8;++slot){
        WORD a=0xb000+(slot/2)*0x1000+((slot%2)*2<<shift);
        // The unused address lines must not select a different nibble.
        a|=sub==2?3:sub==3?12:0;
        MapperWrite(a,value&15);MapperWrite(a+(1<<shift),value>>4);chr_bank(slot,value);
      }
    }
    if(sub==3){
      MapperWrite(0xf000,15);MapperWrite(0xf001,15);MapperWrite(0xf002,7);
      for(unsigned line=0;line<400;++line)MapperHSync();assert(IRQ_State==IRQ_Wiring);
    } else {
      MapperWrite(0xf000,14);MapperWrite(0xf000+(1<<shift),15);
      MapperWrite(0xf000+(2<<shift),2);MapperHSync();assert(IRQ_State==IRQ_Wiring);
      MapperHSync();assert(IRQ_State!=IRQ_Wiring);
      MapperWrite(0xf000+(3<<shift),0);assert(IRQ_State==IRQ_Wiring);
      for(unsigned line=0;line<300;++line)MapperHSync();assert(IRQ_State==IRQ_Wiring);
      // Cycle mode advances by the line's CPU budget; ack preserves APU IRQ.
      MapperWrite(0xf000+(2<<shift),6);MapperHSync();assert(IRQ_State!=IRQ_Wiring);
      APU_Reg[0x15]|=0x40;MapperWrite(0xf000+(3<<shift),0);assert(IRQ_State!=IRQ_Wiring);
      APU_Reg[0x15]&=~0x40;MapperWrite(0xf000+(3<<shift),0);assert(IRQ_State==IRQ_Wiring);
    }
    assert(InfoNES_Reset()==0);for(unsigned line=0;line<400;++line)MapperHSync();assert(IRQ_State==IRQ_Wiring);
    finish();
  }
  // Exercise the CPU path, not just the mapper callbacks: 2 KiB VRC4 RAM
  // mirrors, and VRC2's one-bit latch must not write the generic SRAM first.
  for(unsigned sub:{2,3}){
    rom=banked_cartridge(23,16,true);rom[8]=sub<<4;rom[10]=sub==2?5:0;
    const BYTE program[]={0xa9,0xa5,0x8d,0x07,0x60,0xad,0x07,0x68,0x85,0x04,
                          0xad,0x07,0x70,0x85,0x05,0x4c,0x0f,0x80};
    std::copy(std::begin(program),std::end(program),rom.begin()+16);load(rom);PC=0x8000;SRAMwritten=false;
    if(sub==2)MapperWrite(0x9008,1);
    K6502_Step(100);
    if(sub==2)assert(RAM[4]==0xa5 && RAM[5]==0x70 && SRAMwritten);
    else assert(RAM[4]==0x69 && RAM[5]==0x70 && !SRAMwritten && SRAM[7]==0);
    finish();
  }
  for(unsigned mapper:{15,18,23}){
    rom=banked_cartridge(mapper,1,true);rom[8]=0x10;
    assert(!nes::infones_adapter::load(rom.data(),rom.size()));
    rom[8]=0;rom[10]=8;assert(!nes::infones_adapter::load(rom.data(),rom.size()));
  }
}
static void mapper_ram_permissions() {
  const auto write=[](WORD address,BYTE value){
    const BYTE code[]={0xa9,value,0x8d,BYTE(address),BYTE(address>>8),0x4c,5,2};
    std::copy(std::begin(code),std::end(code),RAM+0x200);
    PC=0x200;SRAMwritten=false;K6502_Step(40);
  };
  const auto read=[](WORD address){
    const BYTE code[]={0xad,BYTE(address),BYTE(address>>8),0x85,4,0x4c,5,2};
    std::copy(std::begin(code),std::end(code),RAM+0x200);
    PC=0x200;K6502_Step(40);return RAM[4];
  };
  for(unsigned mapper:{18,23})for(unsigned sub:{0,2,3})for(unsigned ram:{0,5,7}){
    if(mapper==18 && sub)continue;
    auto rom=banked_cartridge(mapper,1,true);rom[8]=sub<<4;rom[10]=ram;
    assert(nes::infones_adapter::load(rom.data(),rom.size()));
    const auto original=rom;
    const WORD control=mapper==18?0x9ffe:sub==2?0x9008:0x9002;
    const bool gated=mapper==18 || sub==2;
    for(unsigned pass=0;pass<2;++pass){
      // Reset revokes permission without clearing cartridge RAM.
      assert(InfoNES_Reset()==0);SRAM[7]=0x33;
      write(0x6007,0xa5);
      assert(SRAMwritten==bool(ram && !gated));
      assert(SRAM[7]==(ram && !gated?0xa5:0x33));
      if(gated)assert(read(0x6007)==0x60);
      if(!ram && sub==3)assert(read(0x6007)==0x61);
      for(unsigned permission:{3,1,0,2,3}){
        MapperWrite(control,permission);SRAM[7]=0x55;
        const bool readable=ram && (!gated || (permission&1));
        const bool writable=readable && (mapper!=18 || (permission&2));
        write(0x6007,0xa5);
        assert(SRAMwritten==writable && SRAM[7]==(writable?0xa5:0x55));
        assert(read(0x6007)==(readable?SRAM[7]:!ram && sub==3?0x61:0x60));
        if(ram==5){
          assert(read(0x6807)==(readable?SRAM[7]:0x68));
          const bool upper=readable && !(mapper==23 && sub==2);
          assert(read(0x7007)==(upper?SRAM[7]:0x70));
          write(0x7007,0x3c);
          assert(SRAMwritten==bool(writable && upper));
          if(writable && !upper)assert(SRAM[7]==0xa5);
        }
        if(ram==7){
          write(0x7007,0x3c);
          assert(SRAMwritten==writable);
          if(writable)assert(SRAM[0x1007]==0x3c && SRAM[7]==0xa5);
        }
      }
    }
    assert(rom==original);nes::infones_adapter::unload();assert(allocations==0);
  }
}
static void sram_dispatch_lifecycle() {
  // Alternate special and ordinary boards without an explicit unload. The
  // SRAM bus must belong solely to the active cartridge, including Reset.
  static unsigned notifications=0;
  for(unsigned mapper:{18,0,23,268,69,4,18,69,0}) {
    auto rom=banked_cartridge(mapper,1,true);
    if(mapper==0) {
      rom.erase(rom.begin()+16+32768,rom.begin()+16+128*1024);
      rom[4]=2;
    }
    assert(nes::infones_adapter::load(rom.data(),rom.size()));
    const auto original=rom;
    auto expected=mapper==18?Map18_ReadSram:mapper==23?Map23_ReadSram:
                  mapper==69?Map69_ReadSram:nullptr;
    for(unsigned pass=0;pass<2;++pass) {
      assert(MapperReadSram==expected && MapperSramOwnsWrite==bool(expected));
      // An admission failure must preserve both the cartridge and its bus.
      auto bad=banked_cartridge(5,1,true);
      assert(!nes::infones_adapter::load(bad.data(),bad.size()));
      assert(MapperReadSram==expected && MapperSramOwnsWrite==bool(expected));
      if(!expected) {
        MapperSram=+[](WORD address,BYTE data) {
          assert(SRAMBANK[address&0x1fff]==data && SRAMwritten);
          ++notifications;
        };
        const BYTE code[]={0xa9,0xa5,0x8d,7,0x60,0xad,7,0x60,0x85,4,0x4c,10,2};
        std::copy(std::begin(code),std::end(code),RAM+0x200);
        const auto before=notifications;
        SRAMwritten=false;PC=0x200;K6502_Step(40);
        assert(notifications==before+1 && RAM[4]==0xa5 && SRAM[7]==0xa5);
      }
      assert(InfoNES_Reset()==0);
      assert(MapperReadSram==expected && MapperSramOwnsWrite==bool(expected));
    }
    assert(rom==original);
  }
  nes::infones_adapter::unload();
  assert(!MapperReadSram && !MapperSramOwnsWrite && allocations==0);
}
static void mapper8_and_dxrom() {
  auto rom=banked_cartridge(8,4);const auto original=rom;
  assert(nes::infones_adapter::load(rom.data(),rom.size()));
  for(unsigned data=0;data<256;++data)for(WORD address:{0x8000,0x9fff,0xc123,0xffff}){
    MapperWrite(address,data);
    for(unsigned slot=0;slot<4;++slot)assert(ROMBANK[slot]==ROM+(((data>>4)&3)*4+slot)*8192);
    for(unsigned slot=0;slot<8;++slot)assert(PPUBANK[slot]==VROM+((data&3)*8+slot)*1024);
  }
  assert(rom==original);nes::infones_adapter::unload();
  for(unsigned sub:{0,1})for(unsigned mirroring:{0,1,8}){
    rom=banked_cartridge(206,8,true);rom[8]=sub<<4;rom[6]|=mirroring;
    if(sub){rom.erase(rom.begin()+16+32768,rom.begin()+16+128*1024);rom[4]=2;}
    assert(nes::infones_adapter::load(rom.data(),rom.size()));
    const std::array<BYTE *,4> names={PPUBANK[8],PPUBANK[9],PPUBANK[10],PPUBANK[11]};
    if(mirroring==8)for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)assert(names[i]!=names[j]);
    for(unsigned data=0;data<256;++data)for(unsigned reg=0;reg<8;++reg){
      MapperWrite(0x9ffe,reg|0xf8);MapperWrite(0x9fff,data);
      if(reg<2){
        assert(PPUBANK[reg*2]==VROM+(data&62)*1024);
        assert(PPUBANK[reg*2+1]==VROM+((data&62)+1)*1024);
      }else if(reg<6)assert(PPUBANK[reg+2]==VROM+(data&63)*1024);
      else if(!sub)assert(ROMBANK[reg-6]==ROM+(data&15)*8192);
      if(sub)for(unsigned i=0;i<4;++i)assert(ROMBANK[i]==ROM+i*8192);
      else {assert(ROMBANK2==ROM+14*8192);assert(ROMBANK3==ROM+15*8192);}
      const std::array<BYTE *,4> prg={ROMBANK0,ROMBANK1,ROMBANK2,ROMBANK3};
      const auto chr=PPUBANK[0];
      for(WORD address:{0xa000,0xc000,0xe000}){MapperWrite(address,6);MapperWrite(address+1,255-data);}
      for(unsigned i=0;i<4;++i){assert(ROMBANK[i]==prg[i]);assert(PPUBANK[i+8]==names[i]);}
      assert(PPUBANK[0]==chr);
    }
    for(unsigned line=0;line<400;++line)MapperHSync();assert(IRQ_State==IRQ_Wiring);
    assert(InfoNES_Reset()==0);assert(ROMBANK0==ROM && ROMBANK1==ROM+8192);
    nes::infones_adapter::unload();
  }
  for(unsigned mapper:{8,206}){
    auto bad=banked_cartridge(mapper,mapper==8?8:16);
    assert(!nes::infones_adapter::load(bad.data(),bad.size()));
    bad=banked_cartridge(mapper,1);bad.resize(16+256*1024+8192);bad[4]=16;
    assert(!nes::infones_adapter::load(bad.data(),bad.size()));
    bad=banked_cartridge(mapper,0);assert(!nes::infones_adapter::load(bad.data(),bad.size()));
  }
  auto bad=banked_cartridge(206,4,true);bad[8]=0x10;
  assert(!nes::infones_adapter::load(bad.data(),bad.size())); // Fixed PRG must be 32 KiB.
  bad=banked_cartridge(8,4);bad[6]|=4;bad.insert(bad.begin()+16,512,0);
  assert(!nes::infones_adapter::load(bad.data(),bad.size())); // FFE trainer is not a passive copy.
  assert(allocations==0);
}
static void mmc3_appended_chr() {
  // Translations can append banks: 21 x 8 KiB = 168 KiB, not a power of two.
  for(bool nes2:{false,true}) {
    auto rom=banked_cartridge(4,21,nes2);const auto original=rom;
    const char *error=nullptr;
    assert(nes::infones_adapter::load(rom.data(),rom.size(),&error) && !error);
    for(unsigned mode:{0u,0x80u})for(unsigned reg=0;reg<6;++reg)for(unsigned value=0;value<256;++value) {
      MapperWrite(0x8000,mode|reg);MapperWrite(0x8001,value);
      const unsigned slot=(reg<2?reg*2:reg+2)^(mode?4:0);
      const unsigned bank=(reg<2?value&0xfe:value)%168;
      assert(PPUBANK[slot]==VROM+bank*1024 && PPUBANK[slot][0]==bank);
      if(reg<2)assert(PPUBANK[slot+1]==VROM+((bank+1)%168)*1024);
      for(unsigned n=0;n<8;++n)assert(PPUBANK[n]>=VROM && PPUBANK[n]+1024<=VROM+168*1024);
    }
    assert(rom==original);
    nes::infones_adapter::unload();assert(allocations==0);
  }
  auto rom=banked_cartridge(1,21);const char *error=nullptr;
  assert(!nes::infones_adapter::load(rom.data(),rom.size(),&error) && error);
  rom=banked_cartridge(4,33);
  assert(!nes::infones_adapter::load(rom.data(),rom.size(),&error) && error);
}
static void newly_admitted_boards() {
  const char *error=nullptr;
  // VRC6: 16 KiB / 8 KiB PRG windows, eight independent CHR banks, clean IRQ reset.
  auto rom=banked_cartridge(24,16,true);
  assert(nes::infones_adapter::load(rom.data(),rom.size(),&error));
  for(unsigned value=0;value<256;++value){
    MapperWrite(0x8003,value);
    assert(ROMBANK0==ROM+((value&15)*2%16)*8192 && ROMBANK1==ROM+(((value&15)*2+1)%16)*8192);
    MapperWrite(0xc003,value);assert(ROMBANK2==ROM+((value&31)%16)*8192);
    for(unsigned slot=0;slot<8;++slot){
      MapperWrite((slot<4?0xd000:0xe000)+(slot%4),value);
      assert(PPUBANK[slot]==VROM+(value%128)*1024);
    }
  }
  MapperWrite(0xf000,0xfe);MapperWrite(0xf001,2);
  MapperHSync();assert(IRQ_State==IRQ_Wiring);
  MapperHSync();assert(IRQ_State!=IRQ_Wiring);
  MapperWrite(0xf002,0);assert(IRQ_State==IRQ_Wiring);
  MapperWrite(0xf000,0xff);MapperWrite(0xf001,2);
  assert(InfoNES_Reset()==0);
  for(unsigned n=0;n<300;++n)MapperHSync();
  assert(IRQ_State==IRQ_Wiring);
  nes::infones_adapter::unload();assert(allocations==0);

  // NES 2.0 25.2 decodes A3/A2 only: A0/A1 are mirrors, not another wiring.
  rom=banked_cartridge(25,32,true);rom[8]=0x20;
  const auto original=rom;
  assert(nes::infones_adapter::load(rom.data(),rom.size(),&error));
  assert(MapperNo==25 && SubMapperNo==2 && NesHeader.byReserve[0]==0x20);
  MapperWrite(0x8fff,5);assert(ROMBANK0==ROM+5*8192);
  MapperWrite(0x9004,2);assert(ROMBANK0==ROM+14*8192 && ROMBANK2==ROM+5*8192);
  for(unsigned slot=0;slot<8;++slot)for(unsigned value=0;value<256;++value){
    const WORD address=0xb000+(slot/2)*0x1000+(slot%2)*4;
    MapperWrite(address|3,value&15);MapperWrite((address+8)|3,value>>4);
    assert(PPUBANK[slot]==VROM+value*1024);
  }
  assert(rom==original);
  nes::infones_adapter::unload();assert(allocations==0);
  for(BYTE sub:{1,3,15}){
    rom[8]=sub<<4;assert(!nes::infones_adapter::load(rom.data(),rom.size(),&error));
  }

  // COOLBOY's outer registers must reach the extended mapper, not mapper 12.
  rom=banked_cartridge(268,32,true);
  assert(nes::infones_adapter::load(rom.data(),rom.size(),&error));
  assert(MapperNo==268 && SubMapperNo==0);
  MapperWrite(0x8000,6);MapperWrite(0x8001,5);assert(ROMBANK0==ROM+5*8192);
  MapperWrite(0x8000,0);MapperWrite(0x8001,0xc4);assert(PPUBANK[0]==VROM+196*1024);
  MapperSram(0x6000,0x80);assert(PPUBANK[0]==VROM+68*1024);
  MapperSram(0x6000,0x88);assert(PPUBANK[0]==VROM+196*1024);
  MapperSram(0x6002,3);MapperSram(0x6003,0x14);
  assert(ROMBANK0==ROM+4*8192 && ROMBANK1==ROM+5*8192);
  for(unsigned slot=0;slot<8;++slot)assert(PPUBANK[slot]==VROM+(152+slot)*1024);
  MapperSram(0x6003,0x80); // Lock outer registers; subsequent writes are ignored.
  auto bank=PPUBANK[0];MapperSram(0x7ffc,0);assert(PPUBANK[0]==bank);
  assert(nes::infones_adapter::load(rom.data(),rom.size(),&error));
  assert(ROMBANK0==ROM && PPUBANK[0]==VROM);
  nes::infones_adapter::unload();assert(allocations==0);
  rom[8]=0x11;assert(!nes::infones_adapter::load(rom.data(),rom.size(),&error));
  for(unsigned mapper:{24,25,268}){
    rom=banked_cartridge(mapper,0,true);rom[11]=7;
    assert(!nes::infones_adapter::load(rom.data(),rom.size(),&error));
  }
  for(unsigned mapper:{24,25}){
    rom=banked_cartridge(mapper,1,true);rom[4]=32;rom.resize(16+512*1024+8192);
    assert(!nes::infones_adapter::load(rom.data(),rom.size(),&error));
  }
}
static void sunsoft69() {
  auto rom=banked_cartridge(69,16,true);const char *error=nullptr;
  assert(nes::infones_adapter::load(rom.data(),rom.size(),&error) && !error);
  const auto command=[](BYTE reg,BYTE value){MapperWrite(0x8000,reg);MapperWrite(0xa000,value);};
  for(unsigned value=0;value<256;++value) {
    for(unsigned slot=0;slot<8;++slot) {
      command(slot,value);assert(PPUBANK[slot]==VROM+(value%128)*1024);
      assert(PPUBANK[slot][0]==value%128);
    }
    for(unsigned slot=0;slot<3;++slot) {
      command(9+slot,value);assert(ROMBANK[slot]==ROM+(value%16)*8192);
      assert(ROMBANK[slot][0]==value%16);
    }
    command(8,value);
    assert(SRAMBANK==(value&0x40?SRAM:ROM+((value&0x3f)%16)*8192));
  }
  for(unsigned mode=0;mode<4;++mode) {
    command(12,mode);
    const unsigned banks[4][4]={{0,1,0,1},{0,0,1,1},{0,0,0,0},{1,1,1,1}};
    for(unsigned slot=0;slot<4;++slot)
      assert(PPUBANK[NAME_TABLE0+slot]==PPURAM+(NAME_TABLE0+banks[mode][slot])*1024);
  }
  // Exercise the actual CPU bus, including attempted stores to ROM at $6000.
  const BYTE code[]={0x78,0xa9,0x5a,0x8d,0x00,0x60,0xad,0x00,0x60,0x85,0x20,0x4c,0x0b,0xe0};
  std::copy(std::begin(code),std::end(code),rom.begin()+16+15*8192);
  const auto original=rom;
  const auto access=[&](BYTE bank,BYTE expected) {
    command(8,bank);PC=0xe000;K6502_Step(100);
    assert(RAM[0x20]==expected && rom==original);
  };
  access(0xc0,0x5a);assert(SRAM[0]==0x5a);
  access(0x83,3);assert(SRAM[0]==0x5a);
  access(0x40,0x60);assert(SRAM[0]==0x5a);
  // IRQ count enable and IRQ output enable are independent; any control
  // write acknowledges the mapper, preserving an unrelated APU interrupt.
  const auto counter=[&](unsigned count){command(14,count&255);command(15,count>>8);};
  counter(1);command(13,1);
  for(unsigned n=0;n<4;++n)MapperHSync();
  assert(IRQ_State==IRQ_Wiring); // Output enabled, counter stopped.
  command(13,0x80);MapperHSync();assert(IRQ_State==IRQ_Wiring);
  counter(STEP_PER_SCANLINE);command(13,0x81);
  MapperHSync();assert(IRQ_State==IRQ_Wiring); // At zero, not yet underflowed.
  MapperHSync();assert(IRQ_State!=IRQ_Wiring);
  command(13,0x81);assert(IRQ_State==IRQ_Wiring);
  MapperHSync();assert(IRQ_State==IRQ_Wiring); // Counter wrapped, not stuck at zero.
  counter(0);MapperHSync();assert(IRQ_State!=IRQ_Wiring);
  APU_Reg[0x15]|=0x40;command(13,0);assert(IRQ_State!=IRQ_Wiring);
  APU_Reg[0x15]&=~0x40;command(13,0);assert(IRQ_State==IRQ_Wiring);
  nes::infones_adapter::unload();assert(allocations==0);
  rom=banked_cartridge(69,0,true);rom[11]=7;
  assert(!nes::infones_adapter::load(rom.data(),rom.size(),&error) && error);
}
static void discrete_board_registers() {
  auto load=[](unsigned mapper,unsigned prg16,unsigned chr8,unsigned sub=0){
    std::vector<BYTE> rom(16+prg16*16384+chr8*8192,0xff);
    std::fill_n(rom.data(),16,0);std::memcpy(rom.data(),"NES\x1a",4);
    rom[4]=prg16;rom[5]=chr8;rom[6]=(mapper&15)<<4;rom[7]=mapper&0xf0;
    if(sub){rom[7]|=8;rom[8]=sub<<4;}
    if(mapper==242)rom[6]|=2;
    return rom;
  };
  auto start=[](std::vector<BYTE>&rom){
    const char *error=nullptr;assert(nes::infones_adapter::load(rom.data(),rom.size(),&error));
  };
  auto finish=[]{nes::infones_adapter::unload();assert(allocations==0);};
  // Exercise SRAM-window dispatch through the actual CPU, including dirty state.
  auto cpu_write=[](WORD address,BYTE value){
    const BYTE code[]={0xa9,value,0x8d,BYTE(address),BYTE(address>>8),0x4c,5,2};
    std::copy(std::begin(code),std::end(code),RAM+0x200);
    PC=0x200;SRAMwritten=false;K6502_Step(40);
  };
  auto cpu_read=[](WORD address){
    const BYTE code[]={0xad,BYTE(address),BYTE(address>>8),0x85,4,0x4c,5,2};
    std::copy(std::begin(code),std::end(code),RAM+0x200);
    PC=0x200;K6502_Step(40);return RAM[4];
  };
  auto pb=[](unsigned slot,unsigned page){assert(ROMBANK[slot]==ROM+page*8192);};
  auto cb=[](unsigned slot,unsigned page){assert(PPUBANK[slot]==VROM+page*1024);};
  auto mir=[](unsigned mode){
    const unsigned pages[][4]={{0,0,1,1},{0,1,0,1},{1,1,1,1},{0,0,0,0},{0,1,2,3},{0,0,0,1}};
    for(unsigned n=0;n<4;++n)assert(PPUBANK[8+n]==PPURAM+(8+pages[mode][n])*1024);
  };
  // Fixed-PRG latches: both swapped bits, register aliases, no SRAM writes.
  {
    auto rom=load(7,16,0,2);rom[11]=7;start(rom);
    // Conflict observes the old mapping, and applies to both bank and CIRAM bits.
    ROMBANK0[0]=0x12;MapperWrite(0x8000,0x17);pb(0,8);mir(2);
    ROMBANK0[0]=0x01;MapperWrite(0x8000,0x17);pb(0,4);mir(3);
    assert(InfoNES_Reset()==0);ROMBANK0[0]=0;MapperWrite(0x8000,0xff);pb(0,0);
    finish();
  }
  for(unsigned m:{87,184}){
    auto rom=load(m,2,4);start(rom);
    for(unsigned v=0;v<256;++v)for(WORD a:{0x6000,0x67ff,0x7fff}){
      MapperSram(a,v);
      if(m==87)cb(0,(((v&1)<<1)|((v&2)>>1))*8);
      else {cb(0,(v&7)*4);cb(4,(((v>>4)&7)|4)*4);}
    }
    assert(MapperSramOwnsWrite);assert(InfoNES_Reset()==0);
    cb(0,0);if(m==184)cb(4,16);
    SRAM[0x123]=0x5a;cpu_write(0x6123,2);
    assert(SRAM[0x123]==0x5a && !SRAMwritten);
    finish();
  }
  for(unsigned m:{79,113,133,240}){
    auto rom=load(m,m==113?16:m==240?32:4,m==133?4:m==79?8:16);start(rom);
    for(unsigned v=0;v<256;++v){
      MapperApu(0x4120,v);
      unsigned prg=m==79?(v>>3)&1:m==113?(v>>3)&7:m==133?(v>>2)&1:v>>4;
      unsigned chr=m==79?v&7:m==113?(v&7)|((v>>3)&8):m==133?v&3:v&15;
      pb(0,prg*4);cb(0,chr*8);
      if(m==113)mir(v&128?1:0);
      BYTE *before=ROMBANK0;
      MapperApu(0x4000,0);assert(ROMBANK0==before);
      if(m!=240){MapperApu(0x4200,0);assert(ROMBANK0==before);}
      if(m==133){MapperWrite(0xc100,v);pb(0,prg*4);}
    }
    finish();
  }
  {
    auto rom=load(86,8,8);start(rom);
    for(unsigned v=0;v<256;++v){
      MapperSram(0x6fff,v);pb(0,((v>>4)&3)*4);cb(0,((v&3)|((v>>4)&4))*8);
      auto before=ROMBANK0;MapperSram(0x7000,~v);assert(ROMBANK0==before);
    }finish();
  }
  {
    auto rom=load(75,16,16);start(rom);
    assert(MapperHSync==Map0_HSync);
    MapperWrite(0xe123,5);MapperWrite(0xf456,7);MapperWrite(0x9fff,6);
    cb(0,21*4);cb(4,23*4);mir(1);
    MapperWrite(0x8000,29);pb(0,29);MapperWrite(0xc000,31);pb(2,31);
    assert(InfoNES_Reset()==0);cb(0,0);cb(4,4);finish();
  }
  for(unsigned m:{76,88}){
    auto rom=load(m,8,16);start(rom);
    for(unsigned reg=0;reg<8;++reg)for(unsigned v=0;v<256;++v){
      MapperWrite(0x9ffe,reg);MapperWrite(0x9fff,v);
      if(reg>=6)pb(reg-6,v&15);
      else if(m==76 && reg>=2)cb((reg-2)*2,(v&63)*2);
      else if(m==88 && reg<2)cb(reg*2,v&62);
      else if(m==88)cb(reg+2,(v&63)|64);
      auto before=PPUBANK[0];MapperWrite(0xc001,0);assert(PPUBANK[0]==before);
    }
    MapperWrite(0x8000,7);assert(InfoNES_Reset()==0);auto before=ROMBANK1;
    MapperWrite(0x8001,5);assert(ROMBANK1==before);finish();
  }
  for(unsigned m:{72,92}){
    auto rom=load(m,m==72?8:16,16);start(rom);
    MapperWrite(0x8000,0xc3);pb(m==72?0:2,6);cb(0,24);
    MapperWrite(0x8000,0xc5);pb(m==72?0:2,6);cb(0,24); // no rising edge
    MapperWrite(0x8000,0);MapperWrite(0x8000,0xc5);pb(m==72?0:2,10);cb(0,40);
    assert(InfoNES_Reset()==0);MapperWrite(0x8000,0xc1);pb(m==72?0:2,2);cb(0,8);
    // Bus conflicts sample the currently mapped ROM, before changing banks.
    ROMBANK0[0]=0;MapperWrite(0x8000,0xff);ROMBANK0[0]=0xff;
    MapperWrite(0x8000,0xc2);pb(m==72?0:2,4);cb(0,16);
    finish();
  }
  for(unsigned sub:{1,3}){
    auto rom=load(78,8,16,sub);start(rom);
    for(unsigned v=0;v<256;++v){
      MapperWrite(0xffff,v);pb(0,(v&7)*2);cb(0,(v>>4)*8);
      mir(sub==3?(v&8?1:0):(v&8?2:3));
    }finish();
  }
  {
    auto rom=load(89,8,16);start(rom);
    for(unsigned v=0;v<256;++v){
      MapperWrite(0x8123,v);pb(0,((v>>4)&7)*2);cb(0,((v&7)|((v>>4)&8))*8);mir(v&8?2:3);
    }finish();
  }
  {
    auto rom=load(97,16,0);start(rom);pb(0,30);pb(2,30);
    const unsigned modes[]={3,0,1,2};
    for(unsigned v=0;v<256;++v){MapperWrite(0xffff,v);pb(2,(v&15)*2);mir(modes[v>>6]);}
    finish();
  }
  {
    auto rom=load(112,32,32);start(rom);assert(MapperHSync==Map0_HSync);
    for(unsigned reg=0;reg<8;++reg)for(unsigned v=0;v<256;++v){
      MapperWrite(0x9ffe,reg|0xc0);MapperWrite(0xbffe,v);
      if(reg<2)pb(reg,v%64);
      else if(reg<4)cb((reg-2)*2,v&254);
      else cb(reg,v);
    }
    MapperWrite(0xe000,1);mir(0);MapperWrite(0xe000,0);mir(1);
    assert(InfoNES_Reset()==0);MapperWrite(0xa000,3);pb(0,3);cb(4,0);finish();
  }
  {
    auto rom=load(242,32,0);start(rom);
    for(unsigned a=0x8000;a<0x8200;++a){MapperWrite(a,0);pb(0,((a>>3)&15)*4);mir(a&2?0:1);}
    finish();
  }
  {
    auto rom=load(41,16,16);start(rom);
    MapperWrite(0x8000,3);cb(0,0); // inner latch disabled
    MapperSram(0x601c,0);pb(0,16);cb(0,96);
    MapperWrite(0x8000,3);cb(0,120);
    MapperSram(0x6000,0);cb(0,24); // preserve inner bits
    MapperWrite(0x8000,0);cb(0,24);
    assert(InfoNES_Reset()==0);MapperWrite(0x8000,3);cb(0,0);finish();
  }
  {
    auto rom=load(243,8,16);start(rom);
    SRAM[0x1101]=0x5a;
    cpu_write(0x7100,4);cpu_write(0x7101,1);
    assert((cpu_read(0x7101)&7)==1);
    assert(SRAM[0x1101]==0x5a && !SRAMwritten);cb(0,16);
    const unsigned regs[]={0,1,2,3,4,5,6,7};
    for(unsigned reg:regs)for(unsigned v=0;v<8;++v){
      MapperApu(0x4100,reg);MapperSram(0x7101,v);
      assert((MapperReadApu(0x4101)&7)==v);
      assert((MapperReadSram(0x7101)&7)==v);
    }
    MapperApu(0x4100,7);
    const unsigned modes[]={5,0,1,3};
    for(unsigned v=0;v<4;++v){MapperApu(0x4101,v*2);mir(modes[v]);}
    assert(InfoNES_Reset()==0);assert((MapperReadApu(0x4101)&7)==0);cb(0,0);pb(0,0);finish();
  }
  {
    auto rom=load(244,8,8);start(rom);
    MapperWrite(0x8000,0x31);pb(0,4);
    MapperWrite(0xffff,0x1a);cb(0,8);
    MapperWrite(0x8000,0x6a);cb(0,40);
    assert(InfoNES_Reset()==0);cb(0,0);pb(0,0);finish();
  }
  // Failed board admission must not destroy the live cartridge.
  auto live=load(87,2,4);start(live);auto before=ROM;
  for(auto bad:{load(78,8,16),load(87,4,4),load(184,2,8),load(97,16,1),load(76,16,16),
                load(72,16,16),load(133,4,8),load(244,8,16)}){
    const char *error=nullptr;assert(!nes::infones_adapter::load(bad.data(),bad.size(),&error));
    assert(error && ROM==before);
  }
  auto bad242=load(242,32,0);bad242[6]&=~2;
  assert(!nes::infones_adapter::load(bad242.data(),bad242.size()));assert(ROM==before);finish();
  auto large7=load(7,32,0,2);large7[11]=7;
  assert(!nes::infones_adapter::load(large7.data(),large7.size()));
  for(BYTE ram:{BYTE(0x60),BYTE(0x70),BYTE(0x80)}){
    auto r=load(242,32,0);r[7]|=8;r[10]=ram;r[11]=7;
    assert(nes::infones_adapter::load(r.data(),r.size())==(ram==0x70));
    finish();
  }
}
static void bank_switch_boards() {
  auto make=[](unsigned m,unsigned p,unsigned c,unsigned sub=0){
    std::vector<BYTE> r(16+p*16384+c*8192,0xff);
    std::fill_n(r.data(),16,0);std::memcpy(r.data(),"NES\x1a",4);
    r[4]=p;r[5]=c;r[6]=(m&15)<<4;r[7]=m&0xf0;
    if(sub){r[7]|=8;r[8]=sub<<4;r[10]=7;if(!c)r[11]=7;}
    return r;
  };
  auto start=[](std::vector<BYTE>&r){assert(nes::infones_adapter::load(r.data(),r.size()));};
  auto finish=[]{nes::infones_adapter::unload();assert(allocations==0);};
  auto pb=[](unsigned slot,unsigned page){assert(ROMBANK[slot]==ROM+page*8192);};
  auto cb=[](unsigned slot,unsigned page){assert(PPUBANK[slot]==VROM+page*1024);};
  auto mirror=[](bool horizontal){
    for(unsigned n=0;n<4;++n)
      assert(PPUBANK[8+n]==PPURAM+(8+(horizontal?n/2:n%2))*1024);
  };
  auto cpu_write=[](WORD address,BYTE value){
    const BYTE code[]={0xa9,value,0x8d,BYTE(address),BYTE(address>>8),0x4c,5,2};
    std::copy(std::begin(code),std::end(code),RAM+0x200);
    PC=0x200;SRAMwritten=false;K6502_Step(40);
  };
  {
    auto r=make(32,16,32);start(r);
    for(unsigned v=0;v<256;++v){
      MapperWrite(0x8fff,v);MapperWrite(0xafff,v^7);
      for(unsigned mode=0;mode<4;++mode){
        MapperWrite(0x9fff,mode);const unsigned slot=mode&2;
        pb(slot,v&31);pb(slot^2,30);pb(1,(v^7)&31);pb(3,31);mirror(mode&1);
      }
      for(unsigned slot=0;slot<8;++slot){MapperWrite(0xbff8+slot,v);cb(slot,v);}
      MapperWrite(0xc000,0);pb(2,v&31); // ignored range
    }
    cpu_write(0x6000,0x56);assert(SRAM[0]==0x56 && SRAMwritten);
    assert(InfoNES_Reset()==0);pb(0,0);pb(1,1);pb(2,30);pb(3,31);
    assert(MapperHSync==Map0_HSync);finish();
  }
  // BNROM: coherent reset bank, ROM bus conflicts, no NINA registers.
  for(unsigned sub:{0,2})for(unsigned size:{2,8,32,64}){
    auto r=make(34,size,0,sub);start(r);pb(0,size*2-4);
    PPURAM[17]=0x63;
    for(unsigned v=0;v<256;++v){
      MapperWrite(0x8000,v);
      for(unsigned slot=0;slot<4;++slot)pb(slot,((v*4)%(size*2))+slot);
    }
    ROMBANK0[0x123]=0x01;MapperWrite(0x8123,0xff);
    pb(0,4%(size*2));assert(PPURAM[17]==0x63);
    auto bank=ROMBANK0;cpu_write(0x7ffe,0x35);
    assert(ROMBANK0==bank && SRAM[0x1ffe]==0x35 && SRAMwritten);
    assert(InfoNES_Reset()==0);pb(0,size*2-4);finish();
  }
  // NINA-001: low-address latches write through to RAM; ROM writes ignored.
  for(unsigned sub:{0,1}){
    auto r=make(34,4,8,sub);start(r);
    for(unsigned v=0;v<256;++v){
      cpu_write(0x7ffd,v);pb(0,(v&1)*4);assert(SRAM[0x1ffd]==v && SRAMwritten);
      MapperSram(0x7ffe,v);cb(0,(v&15)*4);
      MapperSram(0x7fff,v^3);cb(4,((v^3)&15)*4);
      MapperWrite(0x8000,0);pb(0,(v&1)*4);
      MapperSram(0x6ffd,0);pb(0,(v&1)*4);
    }
    assert(!MapperSramOwnsWrite);assert(InfoNES_Reset()==0);pb(0,0);cb(0,0);cb(4,4);finish();
  }
  {
    auto r=make(140,8,16);start(r);
    for(unsigned v=0;v<256;++v)for(WORD a:{0x6000,0x6fff,0x7fff}){
      MapperSram(a,v);
      for(unsigned slot=0;slot<4;++slot)pb(slot,((v>>4)&3)*4+slot);
      cb(0,(v&15)*8);
    }
    MapperApu(0x5fff,0);MapperWrite(0xffff,0);pb(0,12);cb(0,120);
    SRAM[0]=0x56;cpu_write(0x6000,0x21);pb(0,8);cb(0,8);
    assert(SRAM[0]==0x56 && !SRAMwritten);
    assert(InfoNES_Reset()==0);pb(0,0);cb(0,0);finish();
  }
  {
    auto r=make(226,64,0);start(r);mirror(true);PPURAM[7]=0x65;
    for(unsigned outer=0;outer<256;++outer)for(unsigned inner=0;inner<256;++inner){
      MapperWrite(0xffff,outer);MapperWrite(0xfffe,inner);
      // 16 KiB page bits: r1.0 / r0.7 / r0.4..0; top bit wraps at 1 MiB.
      unsigned bank=((inner&31)|((inner&128)>>2)|((outer&1)<<6))%64;
      unsigned lo=(inner&32)?bank:(bank&~1u),hi=(inner&32)?bank:(lo+1);
      pb(0,lo*2);pb(1,lo*2+1);pb(2,hi*2);pb(3,hi*2+1);mirror(!(inner&64));
    }
    assert(PPURAM[7]==0x65 && MapperHSync==Map0_HSync);
    assert(InfoNES_Reset()==0);pb(0,0);pb(3,3);mirror(true);finish();
  }
  auto live=make(140,8,16);start(live);auto before=ROM;
  for(auto bad:{make(32,32,16),make(32,8,0),make(32,8,16,1),make(34,8,8),
                make(34,4,0,1),make(34,4,8,2),make(140,16,16),make(226,64,1)}){
    assert(!nes::infones_adapter::load(bad.data(),bad.size()));assert(ROM==before);
  }
  finish();
}
static void vrc2_taito_namco_boards() {
  auto start=[](std::vector<BYTE>&r){assert(nes::infones_adapter::load(r.data(),r.size()));};
  auto finish=[]{nes::infones_adapter::unload();assert(allocations==0);};
  auto pb=[](unsigned slot,unsigned page){assert(ROMBANK[slot]==ROM+page*8192);};
  auto cb=[](unsigned slot,unsigned page){assert(PPUBANK[slot]==VROM+page*1024);};
  auto nt=[](unsigned a,unsigned b,unsigned c,unsigned d){
    unsigned pages[]={a,b,c,d};
    for(unsigned n=0;n<4;++n)assert(PPUBANK[8+n]==PPURAM+(8+pages[n])*1024);
  };
  auto write=[](WORD address,BYTE value){
    const BYTE code[]={0xa9,value,0x8d,BYTE(address),BYTE(address>>8),0x4c,5,2};
    std::copy(std::begin(code),std::end(code),RAM+0x200);
    PC=0x200;SRAMwritten=false;K6502_Step(40);
  };
  auto read=[](WORD address){
    const BYTE code[]={0xad,BYTE(address),BYTE(address>>8),0x85,0x07,0x4c,5,2};
    std::copy(std::begin(code),std::end(code),RAM+0x200);
    PC=0x200;K6502_Step(40);return RAM[7];
  };
  {
    auto r=banked_cartridge(22,16,true);start(r);
    for(unsigned v=0;v<256;++v){
      MapperWrite(0x8fff,v);MapperWrite(0xafff,v^7);
      pb(0,(v&31)%16);pb(1,((v^7)&31)%16);pb(2,14);pb(3,15);
      MapperWrite(0x9fff,v);if(v&1)nt(0,0,1,1);else nt(0,1,0,1);
      for(unsigned slot=0;slot<8;++slot){
        WORD low=0xbffc+(slot/2)*0x1000+(slot%2);
        MapperWrite(low,0xa0|(v&15));MapperWrite(low+2,0x50|(v>>4));
        cb(slot,v>>1); // A0 chooses the tile bank, A1 the nibble.
        MapperWrite(low,0);cb(slot,(v&0xf0)>>1);
      }
    }
    SRAM[0]=0x43;write(0x6000,1);assert(!SRAMwritten && SRAM[0]==0x43);
    assert(read(0x6000)==0x61 && read(0x6100)==0x61 && read(0x7000)==0x70);
    write(0x7000,0);assert(read(0x6000)==0x61);
    write(0x6fff,0);assert(read(0x6100)==0x60);
    IRQ_State=IRQ_Wiring;MapperWrite(0xf003,0xff);MapperHSync();
    assert(IRQ_State==IRQ_Wiring && MapperHSync==Map0_HSync);
    write(0x6000,1);assert(InfoNES_Reset()==0);assert(read(0x6000)==0x60);
    MapperWrite(0xb000,0);cb(0,0);finish();
  }
  for(BYTE ram:{BYTE(5),BYTE(7),BYTE(0x70)}){
    auto r=banked_cartridge(22,1,true);r[10]=ram;if(ram>>4)r[6]|=2;
    start(r);write(0x6007,0x59);assert(SRAMwritten);
    assert(read(0x6007)==0x59);
    if(ram==5)assert(read(0x6807)==0x59);
    write(0x7fff,0x95);assert(SRAMwritten && read(0x7fff)==0x95);finish();
  }
  for(unsigned mapper:{80,207})for(bool battery:{false,true}){
    auto r=banked_cartridge(mapper,16,true);r[10]=battery?0x10:1;
    if(battery)r[6]|=2;
    start(r);if(mapper==207)nt(0,0,0,0);
    for(unsigned v=0;v<256;++v){
      for(unsigned reg=0;reg<2;++reg){
        MapperSram(0x7ef0+reg,v);cb(reg*2,v%128);cb(reg*2+1,(v+1)%128);
      }
      for(unsigned reg=2;reg<6;++reg){MapperSram(0x7ef0+reg,v);cb(reg+2,v%128);}
      for(unsigned reg=0;reg<6;++reg){MapperSram(0x7efa+reg,v);pb(reg/2,v%16);}
      MapperSram(0x7ef6,v);MapperSram(0x7ef7,v);
      if(mapper==207)nt(v>>7,v>>7,v>>7,v>>7);
      else if(v&1)nt(0,1,0,1);else nt(0,0,1,1);
      for(WORD permission:{0x7ef8,0x7ef9}){
        SRAM[7]=0x3c;write(permission,v);assert(!SRAMwritten);
        write(0x7f87,0x53);
        assert(SRAMwritten==(v==0xa3));
        assert(SRAM[7]==(v==0xa3?0x53:0x3c));
        assert(read(0x7f07)==(v==0xa3?0x53:0x7f));
        assert(read(0x7f87)==(v==0xa3?0x53:0x7f));
      }
    }
    write(0x7ef8,0xa3);write(0x6000,0x54);assert(!SRAMwritten && read(0x6000)==0x60);
    write(0x7eff,7);assert(!SRAMwritten);pb(2,7);pb(3,15);
    if(mapper==207){write(0x7ef0,0x80);write(0x7ef1,0);nt(1,1,0,0);}
    BYTE saved[5];assert(MapperBlobSize()==5);MapperSaveBlob(saved);
    write(0x7ef8,0);write(0x7ef0,0);MapperLoadBlob(saved);
    write(0x7fff,0x97);assert(SRAMwritten && read(0x7f7f)==0x97);
    if(mapper==207)nt(1,1,0,0);
    assert(InfoNES_Reset()==0);write(0x7f00,0x57);assert(!SRAMwritten && read(0x7f00)==0x7f);
    if(mapper==207)nt(0,0,0,0);
    assert(MapperHSync==Map0_HSync);finish();
  }
  {
    auto r=banked_cartridge(210,32,true);r[8]=0x20;start(r);nt(0,0,0,0);
    for(unsigned v=0;v<256;++v){
      for(unsigned slot=0;slot<8;++slot){MapperWrite(0x87ff+slot*0x800,v);cb(slot,v);}
      for(unsigned slot=0;slot<3;++slot){MapperWrite(0xe7ff+slot*0x800,v);pb(slot,v%16);}
      switch(v>>6){case 0:nt(0,0,0,0);break;case 1:nt(0,1,0,1);break;
        case 2:nt(0,0,1,1);break;case 3:nt(1,1,1,1);break;}
      for(WORD a:{0xc000,0xc800,0xd000,0xd800,0xf800})MapperWrite(a,v);
      // Namco 340 must never map CHR ROM as writable nametable memory.
      for(unsigned n=8;n<12;++n)assert(PPUBANK[n]==PPURAM+0x2000 || PPUBANK[n]==PPURAM+0x2400);
      pb(3,15);
    }
    const auto original=r;write(0x2006,0x20);write(0x2006,0);write(0x2007,0x43);
    assert(r==original);
    SRAM[0]=0x65;write(0x6000,0x57);assert(!SRAMwritten && SRAM[0]==0x65 && read(0x6000)==0x60);
    assert(InfoNES_Reset()==0);nt(0,0,0,0);pb(0,0);pb(2,14);
    assert(MapperHSync==Map0_HSync);finish();
  }
  for(unsigned mirror:{0,1})for(BYTE ram:{BYTE(0),BYTE(5),BYTE(7),BYTE(0x50),BYTE(0x70)}){
    auto r=banked_cartridge(210,32,true);r[8]=0x10;r[10]=ram;r[6]|=mirror;
    if(ram>>4)r[6]|=2;
    start(r);
    auto fixed_nt=[&]{if(mirror)nt(0,1,0,1);else nt(0,0,1,1);};
    fixed_nt();pb(0,0);pb(1,1);pb(2,14);pb(3,15);
    SRAM[0]=0x65;write(0x6000,0x57);assert(!SRAMwritten && SRAM[0]==0x65);
    assert(read(0x6000)==(ram?0x65:0x60));
    for(unsigned v=0;v<256;++v){
      for(unsigned slot=0;slot<8;++slot){MapperWrite(0x87ff+slot*0x800,v);cb(slot,v);}
      for(unsigned slot=0;slot<3;++slot){MapperWrite(0xe7ff+slot*0x800,v);pb(slot,v%16);}
      fixed_nt();pb(3,15);
      for(WORD a:{0xc000,0xc7ff,0xc800,0xd000,0xdfff}){
        MapperWrite(a,v);
        for(WORD addr:{0x6000,0x67ff,0x6800,0x7fff}){
          const unsigned bytes=(ram==5 || ram==0x50)?2048:8192;
          const unsigned offset=addr&(bytes-1);
          SRAM[offset]=0x56;write(addr,0xa9);
          assert(SRAMwritten==bool(ram && (v&1)));
          assert(SRAM[offset]==(ram && (v&1)?0xa9:0x56));
          assert(read(addr)==(ram?SRAM[offset]:addr>>8));
          if(ram && bytes==2048)assert(read(0x6000+offset)==read(0x7800+offset));
        }
      }
      fixed_nt();
    }
    MapperWrite(0xc000,1);MapperWrite(0xf800,0); // N163 register has no effect.
    write(0x6007,0x71);assert(SRAMwritten==bool(ram));
    const auto original=r;
    write(0x2006,0x20);write(0x2006,0);write(0x2007,0x43);assert(r==original);
    IRQ_State=IRQ_Wiring;MapperWrite(0x5800,0xff);MapperHSync();assert(IRQ_State==IRQ_Wiring);
    for(unsigned reset=0;reset<2;++reset){
      assert(InfoNES_Reset()==0);fixed_nt();pb(0,0);pb(2,14);
      SRAM[0]=0x65;write(0x6000,0x57);assert(!SRAMwritten && SRAM[0]==0x65);
    }
    assert(MapperHSync==Map0_HSync);finish();
  }
  auto live=banked_cartridge(22,1,true);start(live);auto before=ROM;
  for(unsigned mapper:{22,80,207,210})for(unsigned kind=0;kind<7;++kind){
    auto r=banked_cartridge(mapper,16,true);if(mapper==210)r[8]=0x20;
    if(kind==0){r[5]=0;r[11]=7;r.resize(16+128*1024);}
    if(kind==1)r[6]|=8;
    if(kind==2)r[8]=mapper==210?0x30:0x10;
    if(kind==3){r[4]=mapper==210?64:32;r.resize(16+r[4]*16384+r[5]*8192);}
    if(kind==4){if(mapper==210)r[10]=7;else {r[5]=32;r.resize(16+128*1024+256*1024);}}
    if(kind==5)r[11]=7;
    if(kind==6)r[10]=8;
    assert(!nes::infones_adapter::load(r.data(),r.size()));assert(ROM==before);
  }
  for(unsigned sub:{0,3}){
    auto r=banked_cartridge(210,16,true);r[8]=sub<<4;
    assert(!nes::infones_adapter::load(r.data(),r.size()));
  }
  for(unsigned mapper:{80,207}){
    auto r=banked_cartridge(mapper,1,true);r[10]=7;
    assert(!nes::infones_adapter::load(r.data(),r.size()));
  }
  finish();
}
static void namco163() {
  auto start=[](std::vector<BYTE>&r){assert(nes::infones_adapter::load(r.data(),r.size()));};
  auto finish=[]{nes::infones_adapter::unload();assert(allocations==0);};
  auto write=[](WORD a,BYTE v){
    const BYTE code[]={0xa9,v,0x8d,BYTE(a),BYTE(a>>8),0x4c,5,2};
    std::copy(std::begin(code),std::end(code),RAM+0x200);PC=0x200;SRAMwritten=false;K6502_Step(40);
  };
  auto read=[](WORD a){
    const BYTE code[]={0xad,BYTE(a),BYTE(a>>8),0x85,7,0x4c,5,2};
    std::copy(std::begin(code),std::end(code),RAM+0x200);PC=0x200;K6502_Step(40);return RAM[7];
  };
  auto ppu_write=[&](WORD a,BYTE v){PPU_Addr=a;write(0x2007,v);};
  auto ppu_read=[&](WORD a){PPU_Addr=a;read(0x2007);return read(0x2007);};
  for(unsigned sub:{0,2,3,5}){
    auto r=banked_cartridge(19,32,true);r[8]=sub<<4;start(r);
    const auto original=r;
    for(unsigned control:{0,0x40,0x80,0xc0})for(unsigned value=0;value<256;++value){
      MapperWrite(0xefff,control|5);
      for(unsigned slot=0;slot<8;++slot){
        MapperWrite(0x87ff+slot*0x800,value);
        assert(PPUBANK[slot]==(value>=0xe0 && !(control&(slot<4?0x40:0x80))
          ? PPURAM+0x2000+(value&1)*1024 : VROM+value*1024));
      }
      for(unsigned slot=0;slot<4;++slot){
        MapperWrite(0xc7ff+slot*0x800,value);
        assert(PPUBANK[8+slot]==(value>=0xe0?PPURAM+0x2000+(value&1)*1024:VROM+value*1024));
      }
      MapperWrite(0xe7ff,value);assert(ROMBANK0==ROM+(value%16)*8192);
      MapperWrite(0xf7ff,value);assert(ROMBANK2==ROM+(value%16)*8192 && ROMBANK3==ROM+15*8192);
    }
    MapperWrite(0x8000,0xe0);MapperWrite(0x8800,0xe1);MapperWrite(0xe800,0);
    assert(PPUBANK[0]==PPURAM+0x2000 && PPUBANK[1]==PPURAM+0x2400);
    MapperWrite(0xe800,0x40);assert(PPUBANK[0]==VROM+0xe0*1024);
    MapperWrite(0xe800,0);assert(PPUBANK[0]==PPURAM+0x2000); // Re-evaluate existing CHR latches.
    for(unsigned slot=0;slot<4;++slot)MapperWrite(0xc000+slot*0x800,0xe0+(slot&1));
    ppu_write(0x123,0x56);assert(ppu_read(0x2123)==0x56 && ppu_read(0x3123)==0x56);
    ppu_write(0x3523,0x67);assert(ppu_read(0x523)==0x67 && ppu_read(0x2523)==0x67);
    for(unsigned slot=0;slot<4;++slot){
      MapperWrite(0xc000+slot*0x800,3+slot);
      ppu_write(0x2007+slot*0x400,0x87);ppu_write(0x3007+slot*0x400,0x98);
      assert(ppu_read(0x2007+slot*0x400)==3+slot && ppu_read(0x3007+slot*0x400)==3+slot);
    }
    assert(r==original);
    // Switching an NT ROM page back to CIRAM restores its unchanged contents.
    MapperWrite(0xc000,0xe0);assert(ppu_read(0x2123)==0x56);
    MapperWrite(0xd800,0xe1);PPURAM[0x3f00]=0x23;ppu_write(0x2f00,0x87);
    assert(PPURAM[0x3f00]==0x23);
    SRAM[0]=0x52;write(0xf800,0x40);write(0x6000,0x73);
    assert(!SRAMwritten && SRAM[0]==0x52 && read(0x6000)==0x60); // Explicitly absent WRAM.
    assert(InfoNES_Reset()==0);assert(MapperReadApu(0x5800)==0);
    MapperWrite(0xe000,0x40); // Freeze sound phase state while checking all 128 bytes.
    MapperWrite(0xffff,0x80);
    for(unsigned n=0;n<128;++n)MapperApu(0x4fff,n^0x35);
    MapperWrite(0xf800,0x80);
    for(unsigned n=0;n<128;++n)assert(MapperReadApu(0x4800)==(n^0x35));
    assert(MapperReadApu(0x4800)==0x35); // Auto increment wraps and preserves enable.
    MapperWrite(0xf800,9);MapperApu(0x4800,0x59);MapperApu(0x4800,0x68);
    assert(MapperReadApu(0x4800)==0x68 && MapperReadApu(0x4800)==0x68);
    assert(MapperReadApu(0x4700)==0x47);
    assert(InfoNES_Reset()==0);assert(MapperReadApu(0x4800)==0);finish();
  }
  for(BYTE ram:{BYTE(5),BYTE(7),BYTE(0x70)}){
    auto r=banked_cartridge(19,16,true);r[10]=ram;if(ram>>4)r[6]|=2;start(r);
    for(unsigned permission=0;permission<256;++permission)for(unsigned page=0;page<4;++page){
      const WORD a=0x6007+page*0x800;const unsigned offset=(ram==5?7:7+page*0x800);
      SRAM[offset]=0x15;write(0xffff,permission);assert(!SRAMwritten);
      write(a,0x57);const bool allowed=(permission&0x40) && !(permission&(1<<page));
      assert(SRAMwritten==allowed && read(a)==(allowed?0x57:0x15));
    }
    assert(InfoNES_Reset()==0);write(0x6007,0x99);assert(!SRAMwritten);finish();
  }
  // IRQ counts actual elapsed CPU clocks, saturates, and is acknowledged by either byte write.
  for(BYTE timing:{BYTE(0),BYTE(1),BYTE(3)}){
    auto r=banked_cartridge(19,1,true);r[12]=timing;start(r);
    RAM[0x200]=0x4c;RAM[0x201]=0;RAM[0x202]=2;PC=0x200;
    MapperApu(0x5000,0);MapperApu(0x5800,0x80);auto before=K6502_Now();
    K6502_Step(113);MapperHSync();
    assert((unsigned(MapperReadApu(0x5800))<<8|MapperReadApu(0x5000))==0x8000+K6502_Now()-before);
    MapperApu(0x5000,0xfa);MapperApu(0x5800,0xff);assert(IRQ_State==IRQ_Wiring);
    K6502_Step(12);MapperHSync();assert(IRQ_State!=IRQ_Wiring);
    assert(MapperReadApu(0x5000)==0xff && MapperReadApu(0x5800)==0xff);
    MapperApu(0x5000,0xff);assert(IRQ_State==IRQ_Wiring);K6502_Step(120);MapperHSync();
    assert(IRQ_State==IRQ_Wiring); // A saturated counter does not wrap/retrigger.
    MapperApu(0x5800,0);MapperApu(0x5000,0x75);K6502_Step(120);MapperHSync();
    assert(MapperReadApu(0x5000)==0x75 && MapperReadApu(0x5800)==0);
    assert(InfoNES_Reset()==0);assert(MapperReadApu(0x5000)==0 && MapperReadApu(0x5800)==0);
    finish();
  }
  // Compare elapsed-time sound-phase updates with a one-cycle reference,
  // including disabled intervals and changing voice counts.
  {
    auto r=banked_cartridge(19,1,true);start(r);
    std::array<BYTE,128> expected{};
    for(unsigned n=0;n<128;++n)expected[n]=(n*73+19)&255;
    MapperWrite(0xe000,0x40);MapperWrite(0xf800,0x80);
    for(auto v:expected)MapperApu(0x4800,v);
    unsigned divider=0,channel=7;
    RAM[0x200]=0x4c;RAM[0x201]=0;RAM[0x202]=2;PC=0x200;
    for(unsigned iteration=0;iteration<32;++iteration){
      const bool disabled=iteration%5==0;MapperWrite(0xe000,disabled?0x40:0);
      expected[0x7f]=(iteration%8)<<4;MapperWrite(0xf800,0x7f);MapperApu(0x4800,expected[0x7f]);
      const auto before=K6502_Now();K6502_Step(1+iteration*71);const auto elapsed=K6502_Now()-before;
      for(unsigned cycle=0;cycle<elapsed && !disabled;++cycle)if(++divider==15){
        divider=0;auto *v=expected.data()+0x40+channel*8;
        unsigned phase=v[1]|unsigned(v[3])<<8|unsigned(v[5])<<16;
        const unsigned freq=v[0]|unsigned(v[2])<<8|unsigned(v[4]&3)<<16;
        phase=(phase+freq)%((256-(v[4]&0xfc))<<16);
        v[1]=phase;v[3]=phase>>8;v[5]=phase>>16;
        if(channel<=7-(expected[0x7f]>>4&7))channel=7;else --channel;
      }
      MapperHSync();MapperWrite(0xf800,0x80);
      for(auto v:expected)assert(MapperReadApu(0x4800)==v);
    }
    finish();
  }
  auto live=banked_cartridge(19,1,true);start(live);auto before=ROM;
  for(unsigned kind=0;kind<7;++kind){
    auto r=banked_cartridge(19,16,true);
    if(kind==0){r[5]=0;r[11]=7;r.resize(16+128*1024);}
    if(kind==1)r[6]|=8;
    if(kind==2)r[8]=0x60;
    if(kind==3){r[4]=64;r.resize(16+1024*1024+128*1024);}
    if(kind==4){r[5]=64;r.resize(16+128*1024+512*1024);}
    if(kind==5)r[11]=7;
    if(kind==6)r[10]=8;
    assert(!nes::infones_adapter::load(r.data(),r.size()));assert(ROM==before);
  }
  finish();
}
int main(){nrom();dendy_vblank_scroll();mapper48();clipping();cpu_operands();audio_backpressure();simple_mapper_boards();hengge177();discrete_board_registers();bank_switch_boards();medium_mapper_boards();mapper_ram_permissions();sram_dispatch_lifecycle();vrc2_taito_namco_boards();namco163();
  cartridge_loading();lifecycle_and_oom();admitted_bank_bounds();mapper8_and_dxrom();mmc3_appended_chr();sunsoft69();newly_admitted_boards();
  puts("core cartridge regression passed");}
