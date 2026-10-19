#pragma once
#include <cstdint>
#include "nes_palette.hpp"
namespace presentation {
// 9-bit token: six-bit palette entry + three emphasis bits. Transparency is
// already resolved by the core and never interpreted as RGB565 color here.
inline std::uint16_t pixel_token(std::uint16_t pixel,std::uint8_t mask,bool pal){
  unsigned color=pixel&63,emphasis=mask>>5;
  if(mask&1)color&=0x30;
  if(pal)emphasis=(emphasis&4)|((emphasis&1)<<1)|((emphasis&2)>>1);
  return color|(emphasis<<6);
}
inline std::uint16_t token_rgb565(unsigned token){
  const auto base=native_palette[token&63];const auto e=(token>>6)&7;
  unsigned r=(base>>11)&31,g=(base>>5)&63,b=base&31;
  // Approximate analog emphasis using 0.75 attenuation per competing bit.
  if(e&1){g=g*3/4;b=b*3/4;}
  if(e&2){r=r*3/4;b=b*3/4;}
  if(e&4){r=r*3/4;g=g*3/4;}
  return (r<<11)|(g<<5)|b;
}
}
