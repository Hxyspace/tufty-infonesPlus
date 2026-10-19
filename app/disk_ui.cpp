// Adapted from Pimoroni Tufty modules/python/_msc.py, reference c4abbcd.
// See licenses/PIMORONI-LICENSE.
#include "disk_ui.hpp"
#include "disk_fonts.hpp"
#include <algorithm>
#include <cstddef>

namespace disk_ui {
namespace {
constexpr int kWidth=160,kHeight=120,kStars=500;
struct Star {float x,y; uint8_t age;};
Star stars[kStars];
uint32_t random_state=0x2350;
bool initialized=false;
uint32_t random() {
    random_state^=random_state<<13; random_state^=random_state>>17; random_state^=random_state<<5;
    return random_state;
}
Star spawn() {return {float(int(random()%161)-80),float(int(random()%121)-60),0};}
constexpr uint16_t rgb(int r,int g,int b) {return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
void pixel(uint16_t *out,int x,int y,uint16_t color) {
    if(x<0 || x>=kWidth || y<0 || y>=kHeight) return;
    size_t at=y*2*320+x*2;
    out[at]=out[at+1]=out[at+320]=out[at+321]=color;
}
int text_width(const char *text,const disk_art::Font &font) {
    int width=0;
    for(;*text;++text) if(*text>=32 && *text<127) width+=font.advance[*text-32];
    return width;
}
void text(uint16_t *out,const char *label,int x,int y,uint16_t color,const disk_art::Font &font) {
    for(;*label;++label) {
        unsigned c=static_cast<unsigned char>(*label);
        if(c<32 || c>=127) continue;
        const uint16_t *rows=font.rows+(c-32)*font.height;
        for(int yy=0;yy<font.height;++yy)
            for(int xx=0;xx<font.width;++xx)
                if(rows[yy]&(0x8000u>>xx)) pixel(out,x+xx,y+yy,color);
        x+=font.advance[c-32];
    }
}
void center(uint16_t *out,const char *label,int y,uint16_t color,const disk_art::Font &font) {
    text(out,label,(kWidth-text_width(label,font))/2,y,color,font);
}
}
bool draw(uint16_t *out,State state,uint32_t elapsed_ms,Service service) {
    if(!initialized) {for(auto &star:stars) star=spawn(); initialized=true;}
    // PSRAM rendering must not hold up the next MSC data/status transaction.
    for(int y=0;y<240;y+=8) {
        std::fill(out+y*320,out+(y+8)*320,0);
        if(service && !service()) return false;
    }
    bool transferring=state==State::Transferring;
    // Match the original 160x120 screen, doubled to panel pixels. Limit catch-up
    // after long Flash operations so a single frame cannot empty the starfield.
    float movement=std::min<uint32_t>(elapsed_ms,100)/(transferring?500.0f:1000.0f);
    unsigned star_count=0;
    for(auto &star:stars) {
        star.x+=star.x*movement; star.y+=star.y*movement;
        if(star.x < -80 || star.x > 80 || star.y < -60 || star.y > 60) star=spawn();
        else if(star.age<50) ++star.age;
        int brightness=star.age*(transferring?255:100)/50;
        if(int(star.x) && int(star.y)) pixel(out,int(star.x+80),int(star.y+60),rgb(brightness,brightness,brightness));
        if(service && ++star_count%32==0 && !service()) return false;
    }
    constexpr uint16_t white=0xffff,orange=rgb(255,165,0),faded=rgb(184,192,200);
    center(out,"USB Disk Mode",5,white,disk_art::absolute);
    text(out,"1:",10,23,white,disk_art::absolute);
    text(out,"2:",10,45,white,disk_art::absolute);
    text(out,"3:",10,67,white,disk_art::absolute);
    if(service && !service()) return false;
    bool valid=state!=State::Unavailable && state!=State::Failed;
    text(out,valid?"Your badge is now":"USB disk is not",30,24,orange,disk_art::ark);
    text(out,valid?"mounted as a disk":"available",30,32,orange,disk_art::ark);
    if(service && !service()) return false;
    text(out,valid?"Copy NES games":"Keep your files;",30,46,orange,disk_art::ark);
    text(out,valid?"into roms/":"do not format",30,54,orange,disk_art::ark);
    if(service && !service()) return false;
    text(out,valid?"Eject the disk to":"Reset to return",30,68,orange,disk_art::ark);
    text(out,valid?"return to games":"to games",30,76,orange,disk_art::ark);
    if(service && !service()) return false;
    const char *status=transferring?"Transferring data!":state==State::Unavailable?"Invalid FAT volume":
        state==State::Failed?"Disk I/O error":state==State::Ejected?"Returning to games...":"Waiting for data";
    center(out,status,102,transferring?white:faded,disk_art::ark);
    return true;
}
}
