#pragma once
#include "pixels.hpp"
#include <cstdint>
namespace presentation {
// The host supplies one PPUMASK value per scanline. Store emphasis once per
// line and six-bit colors per pixel, preserving all nine token bits. Columns
// are contiguous so the LCD consumer reads consecutive colors, not strided rows.
struct GameFrame {
  static constexpr unsigned width=256,height=240;
  std::uint8_t colors[width][height];
  std::uint16_t emphasis[height];
  void line(unsigned y,const std::uint16_t *pixels,std::uint8_t mask,bool pal){
    emphasis[y]=pixel_token(0,mask,pal)&0x1c0;
    const unsigned color_mask=mask&1?0x30:0x3f;
    for(unsigned x=0;x<width;++x)colors[x][y]=pixels[x]&color_mask;
  }
  std::uint16_t token(unsigned x,unsigned y)const{return colors[x][y]|emphasis[y];}
};
static_assert(sizeof(GameFrame)==256*240+240*2);
}
