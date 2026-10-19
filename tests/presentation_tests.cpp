#include "presentation/frame_clock.hpp"
#include "presentation/pixels.hpp"
#include "presentation/game_frame.hpp"
#include "presentation/frame_rate.hpp"
#include "platform/tufty2350/arena.hpp"
#include "platform/tufty2350/display_timing.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <vector>
int main(){
  using namespace presentation;
  FrameClock clock;
  for(auto cpu:{1789773u,1662607u,1773448u}){
    unsigned dots=cpu==1789773?341*262:cpu==1662607?341*312*5:341*312;
    unsigned divider=cpu==1662607?16:3;
    clock.reset(0,cpu,dots,divider);std::uint32_t now=0;
    for(std::uint64_t i=1;i<=100000;++i){
      now=clock.next(now);assert(now==std::uint32_t(i*dots*1000000/(std::uint64_t(cpu)*divider)));
    }
  }
  clock.reset(0xfffff000,1789773,341*262,3);
  assert(std::int32_t(clock.next(0xfffff000)-0xfffff000)>16000);
  clock.reset(0,1789773,341*262,3);assert(clock.next(100000)==100000);
  for(unsigned color=0;color<64;++color){
    assert(token_rgb565(color)==native_palette[color]);
    assert(pixel_token(0x8000|color,0,false)==color);
    assert(pixel_token(color,1,false)==(color&0x30));
    assert(pixel_token(color,0x20,false)==pixel_token(color,0x40,true));
  }
  // Check every pixel against the original nine-bit encoding, including
  // per-line mask changes, PAL emphasis, grayscale and transparency markers.
  GameFrame frame{};
  std::uint16_t line[256];
  for(bool pal:{false,true})for(unsigned pass=0;pass<2;++pass){
    for(unsigned y=0;y<240;++y){
      for(unsigned x=0;x<256;++x)line[x]=0x8000|((x*7+y*11)&63);
      const auto mask=std::uint8_t(y+pass*240);
      frame.line(y,line,mask,pal);
    }
    GameFrame published;
    std::memcpy(&published,&frame,sizeof frame);
    for(unsigned x=0;x<256;++x)for(unsigned y=0;y<240;++y){
      const auto expected=pixel_token(0x8000|((x*7+y*11)&63),std::uint8_t(y+pass*240),pal);
      assert(published.token(x,y)==expected);
    }
  }
  FrameRate rate;
  assert(rate.sample().fps_x100==0);
  rate.frame(0xfffff000);assert(rate.sample().frames==0);
  std::uint32_t at=0xfffff000;
  for(unsigned n=0;n<600;++n){at+=16667;rate.frame(at);}
  auto sample=rate.sample();
  assert(sample.frames==600 && sample.elapsed_us==600*16667 && sample.fps_x100==5999);
  assert(rate.sample().elapsed_us==0);
  for(unsigned n=0;n<100000;++n){at+=50000;rate.frame(at);}
  sample=rate.sample();
  assert(sample.frames==100000 && sample.elapsed_us==5000000000ull && sample.fps_x100==2000);
  rate={};rate.frame(100);rate.frame(33433);
  assert(rate.sample().fps_x100==3000);
  tufty::panel::Timing timing;timing.configure(true);
  std::uint32_t us=0;unsigned idle=0;
  timing.wait([&]{return us;},[]{return true;},[&]{us+=100;++idle;});
  assert(!timing.status().enabled && timing.status().timed_out && idle==500);
  timing.configure(true);us=0;idle=0;
  timing.wait([&]{return us;},[&]{return idle<2 || idle>=4;},[&]{++us;++idle;});
  assert(timing.status().enabled && !timing.status().timed_out && idle==4);
  timing.configure(true);us=0;
  timing.wait([&]{return us;},[]{return false;},[&]{us+=100;});
  assert(!timing.status().enabled && timing.status().timeouts==2);
  alignas(std::max_align_t) std::array<std::byte,4096> memory;
  tufty::Arena arena(memory.data(),memory.size());
  std::vector<void *> pointers;
  for(unsigned n=1;n<=64;++n){
    auto p=arena.allocate(16);assert(p && arena.contains(p));
    assert(reinterpret_cast<std::uintptr_t>(p)%alignof(std::max_align_t)==0);
    std::memset(p,n,16);pointers.push_back(p);
  }
  assert(!arena.allocate(16));
  for(unsigned n=0;n<64;++n)assert(static_cast<unsigned char *>(pointers[n])[0]==n+1);
  for(unsigned n=0;n<64;n+=2)arena.release(pointers[n]);
  for(unsigned n=1;n<64;n+=2)arena.release(pointers[n]);
  assert(arena.allocate(4096)==memory.data());assert(!arena.allocate(1));
  arena.release(memory.data());assert(!arena.allocate(SIZE_MAX));assert(!arena.allocate(0));
  assert(arena.allocate(4096)==memory.data());
}
