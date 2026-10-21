#include "cartridge.hpp"
#include <cstring>

namespace nes {
bool inspect(const std::uint8_t *p,std::size_t size,Cartridge &out,const char **error) {
  out={};if(error)*error=nullptr;
  auto reject=[&](const char *why){if(error)*error=why;return false;};
  if(!p || size<16 || std::memcmp(p,"NES\x1a",4))return reject("Invalid/truncated NES header");
  if(p[7]&3)return reject("VS/PlayChoice/extended consoles are unsupported");
  const auto format=p[7]&12;
  if(format!=0 && format!=8)return reject("Unknown NES header format");
  Cartridge image;
  image.nes2=format==8;
  image.mapper=(p[6]>>4)|(p[7]&0xf0);
  if(image.nes2){
    image.mapper|=(p[8]&15)<<8;image.submapper=p[8]>>4;
    // Exponent/multiplier and extended bank counts need a wider mapper ABI.
    if(p[9])return reject("Extended/exponent ROM sizes need a wider core ABI");
    if((p[12]&0xfc) || p[13] || p[14] || p[15]>1)
      return reject("NES 2.0 extra hardware, misc ROM or expansion input is unsupported");
    // Multi-region images support NTSC/PAL; choose NTSC consistently for both
    // the core and the application's frame clock, preserving the original header.
    image.timing=(p[12]&3)==2?0:p[12]&3;
  }else{
    // Ambiguous old dumper signatures must not silently rewrite the mapper.
    for(unsigned i=12;i<16;++i)if(p[i])return reject("Dirty iNES reserved bytes; repair header first");
    if(p[8]>1 || (p[9]&~1) || p[10] || p[11])
      return reject("Unsupported/ambiguous iNES RAM or hardware layout");
    image.timing=p[9]&1;
  }
  if(!p[4])return reject("ROM has no PRG data");
  image.prg_offset=16+((p[6]&4)?512:0);
  image.prg_size=std::size_t(p[4])*16384;
  image.chr_size=std::size_t(p[5])*8192;
  // Subtraction avoids overflow even when the caller supplies SIZE_MAX.
  if(size<image.prg_offset || image.prg_size>size-image.prg_offset ||
     image.chr_size>size-image.prg_offset-image.prg_size)
    return reject("Truncated trainer/PRG/CHR data");
  std::memcpy(image.header.data(),p,16);out=image;return true;
}
}
