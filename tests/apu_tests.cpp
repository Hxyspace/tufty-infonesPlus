// Include the actual implementation to compare private oscillator/event state
// without adding a serialization/debug API to the production core.
#ifndef APU_IMPLEMENTATION
#define APU_IMPLEMENTATION "../infones/InfoNES_pAPU.cpp"
#endif
#include APU_IMPLEMENTATION
#include "../infones/InfoNES_pAPU_Vrc7.cpp"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {
bool host_output=true;
int capacity=APU_MAX_SAMPLES_PER_SYNC, fail_at=-1, attempts=0;
unsigned live=0,output_calls=0,sample_count=0,frame_count=0,frame_limit=5;
std::size_t live_bytes=0;
std::uint64_t output_hash=1469598103934665603ull;
std::array<WORD,256*240> pixels{};
std::vector<BYTE> prg(32768),chr(8192);
void hash_bytes(std::uint64_t &h,const void *p,std::size_t n){
  const auto *bytes=static_cast<const BYTE *>(p);
  for(std::size_t i=0;i<n;++i)h=(h^bytes[i])*1099511628211ull;
}
}
WORD NesPalette[64];
unsigned int rom_crc=0;
namespace infones::port {
void *allocate(std::size_t n){
  if(attempts++==fail_at)return nullptr;
  auto *p=static_cast<std::size_t *>(std::malloc(sizeof(std::max_align_t)+n));
  if(!p)return nullptr;
  *p=n;++live;live_bytes+=n;
  return reinterpret_cast<BYTE *>(p)+sizeof(std::max_align_t);
}
void release(void *p){
  if(!p)return;
  auto *base=reinterpret_cast<std::size_t *>(static_cast<BYTE *>(p)-sizeof(std::max_align_t));
  assert(live && live_bytes>=*base);--live;live_bytes-=*base;std::free(base);
}
bool has_large_memory(){return true;}
bool remove_sprite_limit(){return false;}
void service(){}
}
int InfoNES_Menu(){return 0;}
int InfoNES_ReadRom(const char *){return -1;}
void InfoNES_ReleaseRom(){ROM=VROM=nullptr;}
int InfoNES_LoadFrame(){return ++frame_count>=frame_limit?-1:0;}
void InfoNES_PadState(DWORD *a,DWORD *b,DWORD *s){*a=0x49;*b=0;*s=frame_count>=frame_limit?PAD_SYS_QUIT:0;}
void InfoNES_DebugPrint(const char *){}
void InfoNES_MessageBox(const char *,...){}
void InfoNES_Error(const char *,...){}
void InfoNES_SoundInit(){}
int InfoNES_SoundOpen(int,int){return 0;}
void InfoNES_SoundClose(){}
bool InfoNES_HasSoundOutput(){return host_output;}
int InfoNES_GetSoundBufferSize(){return capacity;}
void InfoNES_SoundOutput(int n,BYTE *a,BYTE *b,BYTE *c,BYTE *d,BYTE *e,BYTE *f){
  assert(n>=0 && n<=APU_MAX_SAMPLES_PER_SYNC);++output_calls;sample_count+=n;
  for(const auto *p:{a,b,c,d,e,f})if(p)hash_bytes(output_hash,p,n);
}
void InfoNES_PreDrawLine(int y){InfoNES_SetLineBuffer(pixels.data()+y*256,256);}
void InfoNES_PostDrawLine(int){}

namespace {
// Every mutable pAPU field except output buffers, capability, and immutable
// lookup-table pointers. Register-to-duty pointer consistency is checked below.
template<class F> void visit_state(F f){
  f(ApuCtrl,"ApuCtrl");
  f(ApuCtrlNew,"ApuCtrlNew");
  f(ApuQuality,"ApuQuality");
  f(ApuPulseMagic,"ApuPulseMagic");
  f(ApuTriangleMagic,"ApuTriangleMagic");
  f(ApuNoiseMagic,"ApuNoiseMagic");
  f(ApuSamplesPerSync16,"ApuSamplesPerSync16");
  f(ApuCyclesPerSample,"ApuCyclesPerSample");
  f(ApuSampleRate,"ApuSampleRate");
  f(ApuCycleRate,"ApuCycleRate");
  f(ApuC1a,"ApuC1a");
  f(ApuC1b,"ApuC1b");
  f(ApuC1c,"ApuC1c");
  f(ApuC1d,"ApuC1d");
  f(ApuC1Skip,"ApuC1Skip");
  f(ApuC1Index,"ApuC1Index");
  f(ApuC1EnvPhase,"ApuC1EnvPhase");
  f(ApuC1EnvVol,"ApuC1EnvVol");
  f(ApuC1Atl,"ApuC1Atl");
  f(ApuC1SweepPhase,"ApuC1SweepPhase");
  f(ApuC1Freq,"ApuC1Freq");
  f(ApuC2a,"ApuC2a");
  f(ApuC2b,"ApuC2b");
  f(ApuC2c,"ApuC2c");
  f(ApuC2d,"ApuC2d");
  f(ApuC2Skip,"ApuC2Skip");
  f(ApuC2Index,"ApuC2Index");
  f(ApuC2EnvPhase,"ApuC2EnvPhase");
  f(ApuC2EnvVol,"ApuC2EnvVol");
  f(ApuC2Atl,"ApuC2Atl");
  f(ApuC2SweepPhase,"ApuC2SweepPhase");
  f(ApuC2Freq,"ApuC2Freq");
  f(ApuC3a,"ApuC3a");
  f(ApuC3b,"ApuC3b");
  f(ApuC3c,"ApuC3c");
  f(ApuC3d,"ApuC3d");
  f(ApuC3Skip,"ApuC3Skip");
  f(ApuC3Index,"ApuC3Index");
  f(ApuC3Atl,"ApuC3Atl");
  f(ApuC3Llc,"ApuC3Llc");
  f(ApuC3ReloadFlag,"ApuC3ReloadFlag");
  f(ApuC4a,"ApuC4a");
  f(ApuC4b,"ApuC4b");
  f(ApuC4c,"ApuC4c");
  f(ApuC4d,"ApuC4d");
  f(ApuC4Sr,"ApuC4Sr");
  f(ApuC4Fdc,"ApuC4Fdc");
  f(ApuC4Skip,"ApuC4Skip");
  f(ApuC4Index,"ApuC4Index");
  f(ApuC4Atl,"ApuC4Atl");
  f(ApuC4EnvVol,"ApuC4EnvVol");
  f(ApuC4EnvPhase,"ApuC4EnvPhase");
  f(ApuC5Enable,"ApuC5Enable");
  f(ApuC5Looping,"ApuC5Looping");
  f(ApuC5CurByte,"ApuC5CurByte");
  f(ApuC5DpcmValue,"ApuC5DpcmValue");
  f(ApuC5Freq,"ApuC5Freq");
  f(ApuC5Phaseacc,"ApuC5Phaseacc");
  f(ApuC5Address,"ApuC5Address");
  f(ApuC5CacheAddr,"ApuC5CacheAddr");
  f(ApuC5DmaLength,"ApuC5DmaLength");
  f(ApuC5CacheDmaLength,"ApuC5CacheDmaLength");
  f(ApuMmc5Enable,"ApuMmc5Enable");
  f(ApuMmc5P1a,"ApuMmc5P1a");
  f(ApuMmc5P1c,"ApuMmc5P1c");
  f(ApuMmc5P1d,"ApuMmc5P1d");
  f(ApuMmc5P1Skip,"ApuMmc5P1Skip");
  f(ApuMmc5P1Index,"ApuMmc5P1Index");
  f(ApuMmc5P1EnvPhase,"ApuMmc5P1EnvPhase");
  f(ApuMmc5P1EnvVol,"ApuMmc5P1EnvVol");
  f(ApuMmc5P1Atl,"ApuMmc5P1Atl");
  f(ApuMmc5P1Freq,"ApuMmc5P1Freq");
  f(ApuMmc5P2a,"ApuMmc5P2a");
  f(ApuMmc5P2c,"ApuMmc5P2c");
  f(ApuMmc5P2d,"ApuMmc5P2d");
  f(ApuMmc5P2Skip,"ApuMmc5P2Skip");
  f(ApuMmc5P2Index,"ApuMmc5P2Index");
  f(ApuMmc5P2EnvPhase,"ApuMmc5P2EnvPhase");
  f(ApuMmc5P2EnvVol,"ApuMmc5P2EnvVol");
  f(ApuMmc5P2Atl,"ApuMmc5P2Atl");
  f(ApuMmc5P2Freq,"ApuMmc5P2Freq");
  f(ApuMmc5PcmValue,"ApuMmc5PcmValue");
  f(ApuMmc5Ctrl,"ApuMmc5Ctrl");
  f(ApuMmc5CtrlNew,"ApuMmc5CtrlNew");
  f(ApuVrc6Enable,"ApuVrc6Enable");
  f(ApuVrc6P1a,"ApuVrc6P1a");
  f(ApuVrc6P1b,"ApuVrc6P1b");
  f(ApuVrc6P1c,"ApuVrc6P1c");
  f(ApuVrc6P1Skip,"ApuVrc6P1Skip");
  f(ApuVrc6P1Index,"ApuVrc6P1Index");
  f(ApuVrc6P2a,"ApuVrc6P2a");
  f(ApuVrc6P2b,"ApuVrc6P2b");
  f(ApuVrc6P2c,"ApuVrc6P2c");
  f(ApuVrc6P2Skip,"ApuVrc6P2Skip");
  f(ApuVrc6P2Index,"ApuVrc6P2Index");
  f(ApuVrc6SawRate,"ApuVrc6SawRate");
  f(ApuVrc6SawFreqL,"ApuVrc6SawFreqL");
  f(ApuVrc6SawFreqH,"ApuVrc6SawFreqH");
  f(ApuVrc6SawAccum,"ApuVrc6SawAccum");
  f(ApuVrc6SawStep,"ApuVrc6SawStep");
  f(ApuVrc6SawPhaseAcc,"ApuVrc6SawPhaseAcc");
  f(ApuVrc6FreqCtrl,"ApuVrc6FreqCtrl");
  f(ApuSunsoft5BEnable,"ApuSunsoft5BEnable");
  f(ApuS5B_NoiseSkip,"ApuS5B_NoiseSkip");
  f(ApuS5B_NoiseIndex,"ApuS5B_NoiseIndex");
  f(ApuS5B_NoiseLfsr,"ApuS5B_NoiseLfsr");
  f(ApuS5B_NoiseOut,"ApuS5B_NoiseOut");
  f(ApuS5B_EnvSkip,"ApuS5B_EnvSkip");
  f(ApuS5B_EnvIndex,"ApuS5B_EnvIndex");
  f(ApuS5B_EnvStep,"ApuS5B_EnvStep");
  f(ApuS5B_EnvHold,"ApuS5B_EnvHold");
  f(ApuS5B_EnvDecay,"ApuS5B_EnvDecay");
  f(ApuS5B_EnvShape,"ApuS5B_EnvShape");
  f(ApuC5Reg,"ApuC5Reg");
  f(ApuS5B_Reg,"ApuS5B_Reg");
  f(ApuS5B_Skip,"ApuS5B_Skip");
  f(ApuS5B_Index,"ApuS5B_Index");
  f(ApuDmcIrq,"ApuDmcIrq");
  f(leftSamples16,"leftSamples16");
  f(entertime,"entertime");
  f(cur_event,"cur_event");
  // Slots past cur_event are discarded history and have no future effect.
  for(int i=0;i<cur_event;++i)f(ApuEventQueue[i],"ApuEventQueue[i]");
  for(unsigned i=0;i<0x18;++i)f(APU_Reg[i],"APU_Reg[i]");
  f(IRQ_State,"IRQ_State");
  f(IRQ_Wiring,"IRQ_Wiring");
  f(NMI_State,"NMI_State");
#if INFONES_EXTENDED_MAPPERS
  f(ApuVrc7Enable,"ApuVrc7Enable");
  if(vrc7)f(*vrc7,"vrc7)f(*vrc7");
#endif
}
struct State {
  std::vector<BYTE> bytes;
  State(){visit_state([&](const auto &x,const char *){const auto *p=reinterpret_cast<const BYTE *>(&x);bytes.insert(bytes.end(),p,p+sizeof x);});}
  void restore() const{
    std::size_t at=0;
    visit_state([&](auto &x,const char *){assert(at+sizeof x<=bytes.size());std::memcpy(&x,bytes.data()+at,sizeof x);at+=sizeof x;});
    assert(at==bytes.size());
    ApuC1Wave=pulse_waves[ApuC1DutyCycle>>6];ApuC2Wave=pulse_waves[ApuC2DutyCycle>>6];
    ApuMmc5P1Wave=pulse_waves[ApuMmc5P1DutyCycle>>6];ApuMmc5P2Wave=pulse_waves[ApuMmc5P2DutyCycle>>6];
  }
};
void compare(const State &expected){
  const State actual;
  if(expected.bytes!=actual.bytes){
    std::size_t at=0;
    visit_state([&](const auto &x,const char *name){if(std::memcmp(&x,expected.bytes.data()+at,sizeof x))std::fprintf(stderr,"APU state %s differs at byte %zu, field size %zu\n",name,at,sizeof x);at+=sizeof x;});
    for(std::size_t i=0;i<actual.bytes.size();++i)if(expected.bytes[i]!=actual.bytes[i]){std::fprintf(stderr,"offset %zu: %u != %u\n",i,expected.bytes[i],actual.bytes[i]);if(i>400)break;}
    assert(false && "Output capability changed emulated APU state");
  }
  assert(ApuC1Wave==pulse_waves[ApuC1DutyCycle>>6]);
  assert(ApuC2Wave==pulse_waves[ApuC2DutyCycle>>6]);
}
void prepare(int region,bool output){
  host_output=output;capacity=APU_MAX_SAMPLES_PER_SYNC;
  std::memset(&NesHeader,0,sizeof NesHeader);NesHeader.byRomSize=2;NesHeader.byVRomSize=1;
  std::fill(prg.begin(),prg.end(),0xea);std::fill(chr.begin(),chr.end(),0);
  prg[0]=0x4c;prg[1]=0;prg[2]=0x80;
  for(auto off:{0x7ffa,0x7ffc,0x7ffe}){prg[off]=0;prg[off+1]=0x80;}
  ROM=prg.data();VROM=chr.data();
  InfoNES_SetRegion(region);assert(InfoNES_Init());assert(InfoNES_Reset()==0);
  #ifndef APU_LEGACY_REFERENCE
  assert((wave_buffers!=nullptr)==output);
#endif
  // These phase fields predate this change and are not reset by upstream init.
  // Seed them explicitly for independent runs rather than hiding that fact.
  ApuC3Index=ApuC3Skip=0;ApuVrc6FreqCtrl=0;
  // pAPUInit runs before K6502_Reset; normalize the event clock origin so
  // independent cartridges start with identical preexisting core state.
  entertime=getPassedClocks();
  std::memset(SRAM,0,SRAM_SIZE);
  std::memset(ApuEventQueue,0,sizeof ApuEventQueue);
  frame_count=sample_count=output_calls=0;output_hash=1469598103934665603ull;
  pixels.fill(0);
}
void expansions(){
  mmc5_wave_buffers=static_cast<BYTE (*)[APU_MAX_SAMPLES_PER_SYNC]>(infones::memory::allocate(3*APU_MAX_SAMPLES_PER_SYNC));
  vrc6_wave_buffers=static_cast<BYTE (*)[APU_MAX_SAMPLES_PER_SYNC]>(infones::memory::allocate(3*APU_MAX_SAMPLES_PER_SYNC));
  s5b_wave_buffers=static_cast<BYTE (*)[APU_MAX_SAMPLES_PER_SYNC]>(infones::memory::allocate(3*APU_MAX_SAMPLES_PER_SYNC));
  assert(mmc5_wave_buffers && vrc6_wave_buffers && s5b_wave_buffers);
  ApuMmc5Enable=ApuVrc6Enable=ApuSunsoft5BEnable=1;
  ApuMmc5PcmValue=0;
#if INFONES_EXTENDED_MAPPERS
  vrc7_wave_buffer=static_cast<BYTE *>(infones::memory::allocate(APU_MAX_SAMPLES_PER_SYNC));assert(vrc7_wave_buffer);
  ApuVrc7Reset();ApuVrc7Enable=1;
#endif
}
void cleanup(){InfoNES_Fin();assert(live==0 && live_bytes==0);}
#ifndef APU_LEGACY_REFERENCE
void differential(){
  std::mt19937 random(0x4e4553);
  constexpr BYTE types[]={0,1,2,3,4,5,6,7,8,9,10,11,12,14,15,16,17,18,19,APUET_W_CTRL,
    APUET_W_MMC5_P1A,APUET_W_MMC5_P1C,APUET_W_MMC5_P1D,APUET_W_MMC5_P2A,APUET_W_MMC5_P2C,APUET_W_MMC5_P2D,APUET_W_MMC5_CTRL,
    APUET_W_VRC6_P1A,APUET_W_VRC6_P1B,APUET_W_VRC6_P1C,APUET_W_VRC6_P2A,APUET_W_VRC6_P2B,APUET_W_VRC6_P2C,
    APUET_W_VRC6_SAWA,APUET_W_VRC6_SAWB,APUET_W_VRC6_SAWC,APUET_W_VRC6_FREQ};
  for(int region:{INFONES_REGION_NTSC,INFONES_REGION_PAL,INFONES_REGION_DENDY}){
    prepare(region,true);expansions();
    for(unsigned step=0;step<1800;++step){
      unsigned n=step%80==0?882:step%17==0?0:random()%8;
      ApuSamplesPerSync16=(n<<16)|(random()&0xffff);leftSamples16=0;
      // Events exactly on and around the renderer's strict '<' cutoff.
      const int cutoff=ApuCyclesPerSample*(n+1);
      cur_event=step%3==0?APU_EVENT_MAX:random()%24;
      for(int i=0;i<cur_event;++i){
        auto &event=ApuEventQueue[i];event.type=types[random()%std::size(types)];event.data=random();
        event.time=short(i%5==0?cutoff: i%5==1?cutoff+1: i%5==2?cutoff-1:random()%(cutoff+1));
      }
      std::stable_sort(ApuEventQueue,ApuEventQueue+cur_event,[](auto a,auto b){return a.time<b.time;});
      for(int reg=0;reg<14;++reg)ApuWriteSunsoft5B(reg,random());
      // Cover nonzero phases, wraparound and masked accumulators.
      if(step%19==0){ApuC1Index=ApuC2Index=ApuC3Index=0x1fffffffu;}
      const State before;
      sound_output=true;capacity=step%3==0?0:step%3==1?-10:1;
      InfoNES_pAPUHsync(step%2);if(step%5==0)InfoNES_pAPUVsync();
      const State expected;
      // Silent processing must not touch any sample storage. Test the actual
      // dispatcher with a null base buffer, as on the device.
      std::memset(wave_buffers,0xa5,5*APU_MAX_SAMPLES_PER_SYNC);
      std::memset(mmc5_wave_buffers,0xa5,3*APU_MAX_SAMPLES_PER_SYNC);
      std::memset(vrc6_wave_buffers,0xa5,3*APU_MAX_SAMPLES_PER_SYNC);
      std::memset(s5b_wave_buffers,0xa5,3*APU_MAX_SAMPLES_PER_SYNC);
      auto *base=wave_buffers;wave_buffers=nullptr;
      before.restore();const unsigned calls=output_calls;sound_output=false;
      InfoNES_pAPUHsync(step%2);if(step%5==0)InfoNES_pAPUVsync();
      assert(output_calls==calls);compare(expected);wave_buffers=base;
      for(auto *b:{wave_buffers,mmc5_wave_buffers,vrc6_wave_buffers,s5b_wave_buffers}){
        const auto size=(b==wave_buffers?5:3)*APU_MAX_SAMPLES_PER_SYNC;
        for(int i=0;i<size;++i)assert(reinterpret_cast<BYTE *>(b)[i]==0xa5);
      }
    }
    cleanup();
  }
  std::puts("APU differential: 5400 HSync/VSync cases across NTSC/PAL/Dendy passed");
}
#endif
// Run an actual 6502 cartridge which writes all five channels and reads $4015.
// Digests are also compared to an independently compiled pre-change pAPU.
std::uint64_t machine(bool output,int region){
  prepare(region,output);
  std::vector<BYTE> code={0x78,0xd8,0xa2,0xff,0x9a};
  const auto write=[&](WORD address,BYTE value){code.insert(code.end(),{0xa9,value,0x8d,BYTE(address),BYTE(address>>8)});};
  write(0x4017,0);write(0x4015,0x1f);
  for(unsigned r=0;r<20;++r)write(0x4000+r,BYTE(r==16?0x8f:r==19?4:0x3f+r*7));
  write(0x4015,0x1f);write(0x2001,0x1e);
  const unsigned loop=code.size();
  code.insert(code.end(),{0xad,0x15,0x40,0x85,0x00,0xe6,0x01,0xad,0x01,0x00,0x8d,0x11,0x40,
    0x4c,BYTE(loop),BYTE(0x80+(loop>>8))});
  std::copy(code.begin(),code.end(),prg.begin());
  for(unsigned i=0;i<64;++i)NesPalette[i]=i;
  frame_limit=12;InfoNES_Cycle();
  std::uint64_t h=1469598103934665603ull;
  const State state;
  static std::vector<BYTE> reference;
  if(output)reference=state.bytes;else { State expected;expected.bytes=reference;compare(expected); }
  hash_bytes(h,state.bytes.data(),state.bytes.size());
  for(auto range:{std::pair<const void *,std::size_t>{RAM,RAM_SIZE},{SRAM,SRAM_SIZE},{PPURAM,PPURAM_SIZE},{SPRRAM,SPRRAM_SIZE},{pixels.data(),sizeof pixels}})hash_bytes(h,range.first,range.second);
  const auto cycles=K6502_Now();hash_bytes(h,&cycles,sizeof cycles);hash_bytes(h,&PC,sizeof PC);
  assert(frame_count==frame_limit);
#ifndef APU_LEGACY_REFERENCE
  assert(output || output_calls==0);
#endif
  static constexpr std::uint64_t baseline[]={0x0a1e0e55c9ce8d2eull,0x72b3dcbac7228122ull,0xe509cb95316a2838ull};
  // Recorded with the independently compiled pre-optimization implementation.
  assert(h==baseline[region]);
  std::printf("machine region=%d output=%d digest=%016llx samples=%u output_hash=%016llx\n",region,output,(unsigned long long)h,sample_count,(unsigned long long)output_hash);
  cleanup();return h;
}
void lifecycle(){
#ifndef APU_LEGACY_REFERENCE
  std::size_t full=0;
  for(bool output:{true,false,true,false}){
    prepare(INFONES_REGION_NTSC,output);attempts=0;
    if(output)full=live_bytes;else assert(full-live_bytes==5*APU_MAX_SAMPLES_PER_SYNC);
    // Capability is sampled only during init; a host change mid-run cannot
    // make a null waveform allocation become an output buffer.
    host_output=!output;InfoNES_pAPUHsync(false);assert(output || !output_calls);
    cleanup();
  }
  for(bool output:{false,true}){
    prepare(INFONES_REGION_NTSC,output);attempts=0;assert(InfoNES_Reset()==0);const int points=attempts;cleanup();
    for(int fail=0;fail<points;++fail){
      fail_at=-1;prepare(INFONES_REGION_NTSC,output);attempts=0;fail_at=fail;
      assert(InfoNES_Reset()<0);cleanup();fail_at=-1;
    }
  }
#endif
}
} // namespace
int main(){
#ifdef APU_LEGACY_REFERENCE
  // Legacy always materializes output; compare reference digests externally.
  for(int region:{INFONES_REGION_NTSC,INFONES_REGION_PAL,INFONES_REGION_DENDY})machine(true,region);
#else
  for(int region:{INFONES_REGION_NTSC,INFONES_REGION_PAL,INFONES_REGION_DENDY}){
    const auto reference=machine(true,region);assert(machine(false,region)==reference);
  }
  differential();lifecycle();
#endif
  return 0;
}
