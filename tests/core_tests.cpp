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
    std::vector<BYTE> rom(16+prg_banks*16384+chr_banks*8192);
    std::memcpy(rom.data(),"NES\x1a",4);rom[4]=prg_banks;rom[5]=chr_banks;
    rom[6]=(mapper&15)<<4;rom[7]=mapper&0xf0;
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
int main(){nrom();dendy_vblank_scroll();mapper48();clipping();cpu_operands();audio_backpressure();simple_mapper_boards();hengge177();medium_mapper_boards();mapper_ram_permissions();sram_dispatch_lifecycle();
  cartridge_loading();lifecycle_and_oom();admitted_bank_bounds();mapper8_and_dxrom();mmc3_appended_chr();sunsoft69();newly_admitted_boards();
  puts("core cartridge regression passed");}
