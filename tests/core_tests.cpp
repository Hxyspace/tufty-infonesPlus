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
int InfoNES_GetSoundBufferSize(){return sound_capacity;}
void InfoNES_SoundOutput(int n,BYTE *a,BYTE *,BYTE *,BYTE *,BYTE *,BYTE *){assert(n>=0&&n<=APU_MAX_SAMPLES_PER_SYNC);samples+=n; if(n)assert(a);}
void InfoNES_PreDrawLine(int y){assert(y>=0&&y<240);InfoNES_SetLineBuffer(frame.data()+y*256,256);}
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
static void audio_backpressure() {
  struct Snapshot {std::array<BYTE,RAM_SIZE> ram;int dmc;std::uint32_t cycles;ApuDmcIrq_t irq;};
  const auto run=[](int capacity,bool mute){
    image(0);assert(InfoNES_Init());assert(InfoNES_Reset()==0);
    // DMC interrupt, fastest rate, 65-byte sample from $C000.
    const BYTE code[]={0xa9,0x8f,0x8d,0x10,0x40,0xa9,0,0x8d,0x12,0x40,
                       0xa9,4,0x8d,0x13,0x40,0xa9,0x10,0x8d,0x15,0x40,
                       0x4c,0x14,0x80};
    std::copy(std::begin(code),std::end(code),prg.begin());
    sound_capacity=capacity;APU_Mute=mute;frames=samples=0;InfoNES_Cycle();
    Snapshot s{};std::copy_n(RAM,RAM_SIZE,s.ram.begin());s.dmc=ApuC5DmaLength;
    s.cycles=K6502_Now();s.irq=ApuDmcIrq;
    if(capacity<=0)assert(samples==0);else assert(samples>0);
    InfoNES_Fin();return s;
  };
  const auto reference=run(APU_MAX_SAMPLES_PER_SYNC,false);
  for(auto capacity:{0,-10,1,APU_MAX_SAMPLES_PER_SYNC})for(bool mute:{false,true}){
    const auto s=run(capacity,mute);
    assert(s.ram==reference.ram && s.dmc==reference.dmc && s.cycles==reference.cycles);
    assert(std::memcmp(&s.irq,&reference.irq,sizeof s.irq)==0);
  }
  sound_capacity=APU_MAX_SAMPLES_PER_SYNC;APU_Mute=0;
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
    unsigned chr_banks=mapper==2 || mapper==7?0:large && mapper!=0?mapper==3?4:32:1;
    std::vector<BYTE> rom(16+prg_banks*16384+chr_banks*8192);
    std::memcpy(rom.data(),"NES\x1a",4);rom[4]=prg_banks;rom[5]=chr_banks;
    rom[6]=(mapper&15)<<4;rom[7]=mapper&0xf0;
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
  for(unsigned n=0;n<prg_bytes/8192;++n)std::fill_n(rom.data()+16+n*8192,8192,n);
  for(unsigned n=0;n<chr_banks*8;++n)std::fill_n(rom.data()+16+prg_bytes+n*1024,1024,n);
  return rom;
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
int main(){nrom();mapper48();clipping();cpu_operands();audio_backpressure();
  cartridge_loading();lifecycle_and_oom();admitted_bank_bounds();mmc3_appended_chr();sunsoft69();
  puts("core cartridge regression passed");}
