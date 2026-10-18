#include "adapters/infones/cartridge.hpp"
#include "InfoNES.h"
#include "InfoNES_Mapper.h"
#include "InfoNES_Port.h"
#include "InfoNES_Region.h"
#include <cstring>

namespace nes::infones_adapter {
bool validate_board(const Cartridge &i,const char **error) {
  if(error)*error=nullptr;
  auto reject=[&](const char *s){if(error)*error=s;return false;};
  bool found=false;
  for(unsigned m=0;MapperTable[m].nMapperNo!=-1;++m)
    if(MapperTable[m].nMapperNo==i.mapper)found=true;
  if(!found)return reject("Unimplemented mapper; no legacy-number fallback");
  if(i.submapper)return reject("Submapper needs board-specific validation");
  const auto power_of_two=[](std::size_t n){return n && !(n&(n-1));};
  if(!power_of_two(i.prg_size) || (i.chr_size && !power_of_two(i.chr_size)) ||
     i.prg_size>512*1024 || i.chr_size>256*1024)
    return reject("Mapper bank geometry needs explicit validation");
  // Initial public loader contract. Other implementations stay in nes_core
  // and must gain geometry/RAM/write-path tests before being admitted here.
  bool chr_required=false;
  switch(i.mapper){
    case 0:
      if(i.prg_size>32768 || i.chr_size>8192)return reject("Invalid NROM geometry");
      break;
    case 1:case 4:case 7:break;
    case 2:if(i.chr_size)return reject("UxROM requires CHR RAM");break;
    case 3:
      if(i.prg_size>32768 || i.chr_size>32768)return reject("Invalid CNROM geometry");
      chr_required=true;break;
    case 48:case 66:chr_required=true;break;
    default:return reject("Mapper board geometry is not yet admitted by this loader");
  }
  if((i.mapper!=0 && i.mapper!=1 && i.mapper!=3 && i.prg_size<32768) ||
     (chr_required && !i.chr_size))return reject("Invalid mapper PRG/CHR geometry");
  if(i.mapper==7 && i.chr_size)return reject("AxROM requires CHR RAM");
  if(i.nes2){
    const auto ram=[](unsigned n){return n?64u<<n:0u;};
    const auto &h=i.header;
    const auto prg_ram=ram(h[10]&15)+ram(h[10]>>4);
    const auto chr_ram=ram(h[11]&15)+ram(h[11]>>4);
    // Generic SRAM is 8 KiB; simultaneous volatile/nonvolatile layouts are
    // not modeled. CHR ROM + CHR RAM needs mapper-specific bank wiring.
    if(prg_ram>8192 || ((h[10]&15) && (h[10]>>4)) || (h[11]>>4) ||
       (i.chr_size && chr_ram) || (!i.chr_size && chr_ram!=8192))
      return reject("NES 2.0 RAM wiring is unsupported");
    if(bool(h[6]&2)!=bool(h[10]>>4))return reject("NES 2.0 battery/RAM layout mismatch");
  }
  return true;
}
void unload(){InfoNES_Fin();}
bool load(const std::uint8_t *data,std::size_t size,const char **error) {
  Cartridge image;
  if(!inspect(data,size,image,error) || !validate_board(image,error))return false;
  unload();
  // Upstream board/timing heuristics use the PRG+CHR CRC, excluding header
  // and trainer. Keep it stable across iNES/NES 2.0 representations.
  std::uint32_t crc=~0u;
  for(std::size_t n=0;n<image.prg_size+image.chr_size;++n){
    crc^=data[image.prg_offset+n];
    for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((0u-(crc&1))&0xedb88320u);
  }
  InfoNES_RomCrc=~crc;
  InfoNES_SetRegion(image.timing==3?INFONES_REGION_DENDY:
                    image.timing==1?INFONES_REGION_PAL:INFONES_REGION_NTSC);
  if(!InfoNES_Init()){
    if(error)*error="Core RAM allocation failed";return false;
  }
  static_assert(sizeof(NesHeader)==16);
  std::memcpy(&NesHeader,image.header.data(),16);
  std::memset(SRAM,0,SRAM_SIZE);
  if(image.header[6]&4)std::memcpy(SRAM+0x1000,data+16,512);
  ROM=const_cast<BYTE *>(data+image.prg_offset);
  VROM=image.chr_size?ROM+image.prg_size:nullptr;
  if(InfoNES_Reset()<0){
    const bool oom=infones::memory::failed;unload();
    if(error)*error=oom?"Mapper/APU allocation failed":"Core reset failed";
    return false;
  }
  return true;
}
}
