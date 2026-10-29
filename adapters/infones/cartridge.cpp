#include "adapters/infones/cartridge.hpp"
#include "InfoNES.h"
#include "InfoNES_Mapper.h"
#include "InfoNES_Port.h"
#include "InfoNES_Region.h"
#include <cstring>
#include <algorithm>

namespace nes::infones_adapter {
bool validate_board(const Cartridge &i,const char **error) {
  if(error)*error=nullptr;
  auto reject=[&](const char *s){if(error)*error=s;return false;};
  bool found=false;
  for(unsigned m=0;MapperTable[m].nMapperNo!=-1;++m)
    if(MapperTable[m].nMapperNo==i.mapper)found=true;
  if(!found)return reject("Unimplemented mapper; no legacy-number fallback");
  if(std::find(admitted_mappers.begin(),admitted_mappers.end(),i.mapper)==admitted_mappers.end())
    return reject("Mapper board geometry is not yet admitted by this loader");
  // Admit only variants with explicit wiring in the core.
  if(i.submapper && !(((i.mapper==2 || i.mapper==3 || i.mapper==7 || i.mapper==25) && i.submapper==2) ||
                     (i.mapper==23 && (i.submapper==2 || i.submapper==3)) ||
                     (i.mapper==19 && (i.submapper==2 || i.submapper==3 || i.submapper==5)) ||
                     (i.mapper==206 && i.submapper==1) ||
                     (i.mapper==210 && (i.submapper==1 || i.submapper==2)) ||
                     (i.mapper==34 && (i.submapper==1 || i.submapper==2)) ||
                     (i.mapper==78 && (i.submapper==1 || i.submapper==3))))
    return reject("Submapper needs board-specific validation");
  const auto power_of_two=[](std::size_t n){return n && !(n&(n-1));};
  // MMC3 CHR registers wrap by the actual number of 1 KiB banks. Translated
  // images can append CHR banks without rounding the file up to a power of two.
  const bool mmc3_chr=i.mapper==4 && i.chr_size && i.chr_size%8192==0;
  if(!power_of_two(i.prg_size) || (i.chr_size && !power_of_two(i.chr_size) && !mmc3_chr) ||
     i.prg_size>(i.mapper==15 || i.mapper==34 || i.mapper==177 || i.mapper==226 || i.mapper==241?1024u:512u)*1024 || i.chr_size>256*1024)
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
    case 11:
      if(i.prg_size>128*1024 || i.chr_size>128*1024)
        return reject("Color Dreams bank addressing exceeded");
      break;
    case 8:
      if(i.prg_size>128*1024 || i.chr_size>32*1024 || (i.header[6]&4))
        return reject("FFE mapper 8 admission requires fixed GNROM mode without trainer");
      chr_required=true;break;
    case 206:
      if(i.prg_size>128*1024 || i.chr_size>64*1024 ||
         (i.submapper==1 && i.prg_size!=32768))
        return reject("Invalid DxROM bank geometry");
      chr_required=true;break;
    case 15:
      if(i.chr_size)return reject("Mapper 15 requires CHR RAM");
      break;
    case 18:
      if(i.prg_size>256*1024)return reject("SS8806 PRG admission exceeds 256 KiB");
      chr_required=true;break;
    case 19:
      if(i.header[6]&8)return reject("Namco 163 requires banked 2 KiB CIRAM, not four-screen");
      chr_required=true;break;
    case 22:case 80:case 207:
      if(i.prg_size>256*1024 || i.chr_size>128*1024 || (i.header[6]&8))
        return reject("VRC2a/X1-005 requires PRG <= 256 KiB, CHR <= 128 KiB and no four-screen");
      chr_required=true;break;
    case 210:
      if(!i.nes2 || (i.submapper!=1 && i.submapper!=2) || (i.header[6]&8))
        return reject("Mapper 210 requires explicit Namco 175/340 submapper 1/2");
      chr_required=true;break;
    case 23:case 24:case 25:
      if(i.prg_size>256*1024)return reject("VRC PRG exceeds 256 KiB bank addressing");
      chr_required=true;break;
    case 32:
      if(i.prg_size>256*1024 || (i.header[6]&8))
        return reject("G-101 requires PRG <= 256 KiB and H/V mirroring");
      chr_required=true;break;
    case 34:
      if((i.submapper==1 && !i.chr_size) || (i.submapper==2 && i.chr_size) ||
         (i.chr_size && (i.prg_size>64*1024 || i.chr_size>64*1024)) || (i.header[6]&8))
        return reject("Mapper 34 requires BNROM CHR RAM or NINA-001 PRG/CHR <= 64 KiB");
      break;
    case 140:
      if(i.prg_size>128*1024 || i.chr_size>128*1024)
        return reject("JF-11/14 bank addressing exceeded");
      chr_required=true;break;
    case 226:
      if(i.chr_size || (i.header[6]&8))
        return reject("Mapper 226 requires CHR RAM and programmable H/V mirroring");
      break;
    case 33:case 48:case 66:case 69:case 268:chr_required=true;break;
    case 70:
      if(i.prg_size>128*1024 || i.chr_size>128*1024)
        return reject("Bandai 74161 bank addressing exceeded");
      chr_required=true;break;
    case 71:
      if(i.prg_size>256*1024 || i.chr_size)
        return reject("Camerica requires PRG <= 256 KiB and CHR RAM");
      break;
    case 241:
      if(i.chr_size)return reject("Mapper 241 admission requires CHR RAM");
      break;
    case 177:
      if(i.chr_size || (i.header[6]&8))
        return reject("Mapper 177 requires CHR RAM and programmable H/V mirroring");
      break;
    case 41:case 113:
      if(i.prg_size>256*1024 || i.chr_size>128*1024)
        return reject("Discrete board bank addressing exceeded");
      chr_required=true;break;
    case 72:case 78:case 89:
      if(i.prg_size>128*1024 || i.chr_size>128*1024)
        return reject("Discrete board bank addressing exceeded");
      if(i.mapper==78 && (!i.submapper || (i.header[6]&8)))
        return reject("Mapper 78 requires explicit NES 2.0 submapper 1 or 3");
      chr_required=true;break;
    case 75:case 92:
      if(i.prg_size>256*1024 || i.chr_size>128*1024)
        return reject("Discrete board bank addressing exceeded");
      chr_required=true;break;
    case 76:case 88:
      if(i.prg_size>128*1024 || i.chr_size>128*1024)
        return reject("Namco bank addressing exceeded");
      chr_required=true;break;
    case 79:case 133:
      if(i.prg_size>64*1024 || i.chr_size>(i.mapper==79?64u:32u)*1024)
        return reject("Discrete board bank addressing exceeded");
      chr_required=true;break;
    case 86:case 244:
      if(i.prg_size>128*1024 || i.chr_size>64*1024)
        return reject("Discrete board bank addressing exceeded");
      chr_required=true;break;
    case 87:case 184:
      if(i.prg_size!=32768 || i.chr_size>32768)
        return reject("Fixed PRG / CHR latch geometry exceeded");
      chr_required=true;break;
    case 97:
      if(i.prg_size>256*1024 || i.chr_size)
        return reject("TAM-S1 requires PRG <= 256 KiB and CHR RAM");
      break;
    case 112:chr_required=true;break;
    case 240:
      if(i.chr_size>128*1024)return reject("Mapper 240 CHR addressing exceeded");
      chr_required=true;break;
    case 242:
      if(!(i.header[6]&2) || i.chr_size)
        return reject("Mapper 242 admission is limited to battery-backed CHR RAM RPG boards");
      break;
    case 243:
      if(i.prg_size>128*1024 || i.chr_size>128*1024)
        return reject("Sachen SA-020A bank addressing exceeded");
      chr_required=true;break;
    default:return reject("Mapper board geometry is not yet admitted by this loader");
  }
  if((i.mapper!=0 && i.mapper!=1 && i.mapper!=3 && i.prg_size<32768) ||
     (chr_required && !i.chr_size))return reject("Invalid mapper PRG/CHR geometry");
  if(i.mapper==7 && i.chr_size)return reject("AxROM requires CHR RAM");
  if(i.mapper==7 && i.submapper==2 && i.prg_size>256*1024)
    return reject("AxROM bus-conflict board exceeds 256 KiB PRG");
  if(i.nes2){
    const auto ram=[](unsigned n){return n?64u<<n:0u;};
    const auto &h=i.header;
    const auto prg_ram=ram(h[10]&15)+ram(h[10]>>4);
    const auto chr_ram=ram(h[11]&15)+ram(h[11]>>4);
    if((i.mapper==80 || i.mapper==207) && prg_ram && prg_ram!=128)
      return reject("X1-005 has 128 bytes of internal RAM");
    if(i.mapper==210 && i.submapper==2 && prg_ram)
      return reject("Namco 340 has no PRG RAM");
    if(i.mapper==177 && prg_ram!=8192)
      return reject("Mapper 177 admission requires 8 KiB WRAM");
    if(i.mapper==242 && prg_ram!=8192)
      return reject("Mapper 242 RPG admission requires 8 KiB WRAM");
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
