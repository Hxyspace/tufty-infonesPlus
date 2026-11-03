#include "game_view.hpp"
#include <algorithm>
#include <cstring>

namespace presentation {
namespace {
// Five columns, seven bits per column; limited to the HUD's labels and numbers.
constexpr char letters[]="0123456789.NESLCD-";
constexpr std::uint8_t glyphs[][5]={
    {62,65,65,65,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},
    {24,20,18,127,16},{39,69,69,69,57},{62,73,73,73,50},{1,113,9,5,3},
    {54,73,73,73,54},{38,73,73,73,62},{0,96,96,0,0},
    {127,2,4,8,127},{127,73,73,73,65},{38,73,73,73,50},
    {127,64,64,64,64},{62,65,65,65,34},{127,65,65,34,28},{8,8,8,8,8}};
}
void FpsMonitor::reset(Position position,bool enabled) {
    position_=position;enabled_=enabled && position!=Position::Center;started_=valid_=false;
    at_=sequence_=frames_=nes_=lcd_=0;
    if(enabled_) raster();
}
void FpsMonitor::text(unsigned group,unsigned x,const char *value) {
    for(;*value;++value,x+=6) {
        const auto *found=std::strchr(letters,*value);if(!found) continue;
        const auto &glyph=glyphs[found-letters];
        for(unsigned c=0;c<5;++c) for(unsigned r=0;r<7;++r)
            if(glyph[c]&(1u<<r)) mask_[group][r]|=std::uint64_t(1)<<(x+c);
    }
}
void FpsMonitor::raster() {
    std::memset(mask_,0,sizeof mask_);
    text(0,8,"NES");text(1,8,"LCD");
    for(unsigned g=0;g<2;++g) {
        char number[5]="--.-";
        if(valid_) {
            const auto value=std::min<std::uint32_t>(g?lcd_:nes_,999);
            unsigned i=0;
            if(value>=100) number[i++]='0'+value/100;
            number[i++]='0'+value/10%10;
            number[i++]='.';
            number[i++]='0'+value%10;
            number[i]='\0';
        }
        text(g,32,number);
    }
}
void FpsMonitor::completed(std::uint32_t sequence,std::uint32_t now) {
    if(!enabled_) return;
    if(!started_) {started_=true;at_=now;sequence_=sequence;frames_=0;return;}
    ++frames_;
    const auto elapsed=std::uint32_t(now-at_);
    if(elapsed<1000000) return;
    nes_=(std::uint64_t(std::uint32_t(sequence-sequence_))*10000000+elapsed/2)/elapsed;
    lcd_=(std::uint64_t(frames_)*10000000+elapsed/2)/elapsed;
    at_=now;sequence_=sequence;frames_=0;valid_=true;raster();
}
void FpsMonitor::column(unsigned x,std::uint16_t *pixels) const {
    if(!enabled_) return;
    const unsigned left=position_==Position::Left?256:0;
    if(x<left || x>=left+64) return;
    for(unsigned g=0;g<2;++g) for(unsigned y=0;y<7;++y)
        if(mask_[g][y]&(std::uint64_t(1)<<(x-left))) pixels[198+g*16+y]=0xbdf7;
}
}
