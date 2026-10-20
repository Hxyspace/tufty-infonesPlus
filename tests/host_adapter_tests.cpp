#include "core_api/host.hpp"
#include "InfoNES_Port.h"
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
static unsigned frames=0,services=0,allocations=0;
static std::array<unsigned,240> lines;
static int fail_at=-1,attempts=0;
namespace infones::port {
void *allocate(std::size_t n){if(attempts++==fail_at)return nullptr;auto p=std::malloc(n);if(p)++allocations;return p;}
void release(void *p){if(p){assert(allocations);--allocations;}std::free(p);}
bool has_large_memory(){return true;}
bool remove_sprite_limit(){return false;}
}
int main(){
  std::vector<std::uint8_t> rom(16+32768+8192);
  std::memcpy(rom.data(),"NES\x1a",4);rom[4]=2;rom[5]=1;
  rom[16]=0x4c;rom[17]=0;rom[18]=0x80;
  for(unsigned n:{0x7ffa,0x7ffc,0x7ffe}){rom[16+n]=0;rom[17+n]=0x80;}
  nes::Host host;
  host.service=[]{++services;};
  host.input=[](std::uint8_t &a,std::uint8_t &b,bool &quit){a=0x81;b=0;quit=frames>=3;};
  host.line=[](unsigned y,const std::uint16_t *pixels,std::uint8_t mask,bool pal){
    assert(y<240 && pixels && !pal && mask==0);++lines[y];
    for(unsigned x=0;x<256;++x)assert((pixels[x]&0x7fc0)==0);
  };
  host.frame=[]{++frames;const char *error=nullptr;nes::Host nested;
    assert(!nes::infones_adapter::run(nested,nullptr,0,&error) && error);
  };
  const char *error=nullptr;
  assert(nes::infones_adapter::run(host,rom.data(),rom.size(),&error) && !error);
  const int allocation_points=attempts;
  assert(frames==3 && services>700 && allocations==0);
  for(auto n:lines)assert(n==3);
  for(int i=0;i<allocation_points;++i){fail_at=i;attempts=0;
    assert(!nes::infones_adapter::run(host,rom.data(),rom.size(),&error) && error && allocations==0);
  }
  fail_at=-1;frames=0;lines.fill(0);
  assert(nes::infones_adapter::run(host,rom.data(),rom.size(),&error) && allocations==0);
}
