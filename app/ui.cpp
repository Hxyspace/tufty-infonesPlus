#include "ui.hpp"
#include "menu_assets.hpp"
#include "storage.hpp"
#include "battery.hpp"
#include "input.hpp"
#include <algorithm>
#include <cstring>
#ifndef TUFTY_HOST_PREVIEW
#include "pico/stdlib.h"
#else
#define __uninitialized_psram(name)
#endif

namespace ui {
static uint16_t __uninitialized_psram("art") custom_bg[320*240];
namespace {
struct IconSpec {const char *path; unsigned width,height,offset;};
constexpr IconSpec icons[]={
    {"/rom/settings/header.565",30,20,0},
    {"/rom/settings/display.565",20,20,1200},
    {"/rom/settings/control.565",20,20,2000},
    {"/rom/settings/power.565",20,20,2800},
    {"/rom/settings/system.565",20,20,3600},
    {"/rom/settings/about.565",20,20,4400},
    {"/rom/settings/settings.565",58,58,5200}
};
static_assert(sizeof(icons)/sizeof(*icons)==unsigned(Icon::Count));
uint8_t __uninitialized_psram("icons") icon_pixels[11928];
bool icon_valid[unsigned(Icon::Count)]{};
}
static bool have_bg=false,battery_sampled=false;
static uint32_t last_battery_sample=0;
void load_icons() {
    for(unsigned i=0;i<unsigned(Icon::Count);++i) {
        const auto &spec=icons[i]; size_t actual=0;
        const size_t bytes=spec.width*spec.height*2;
        icon_valid[i]=storage::read_file(spec.path,icon_pixels+spec.offset,bytes,actual) && actual==bytes;
    }
}
bool icon(uint16_t *out,Icon which,int x,int y) {
    const auto i=unsigned(which);
    if(i>=unsigned(Icon::Count) || !icon_valid[i]) return false;
    const auto &spec=icons[i];
    for(int yy=std::max(0,-y);yy<std::min(int(spec.height),240-y);++yy)
        for(int xx=std::max(0,-x);xx<std::min(int(spec.width),320-x);++xx) {
            const auto *p=icon_pixels+spec.offset+2*(yy*spec.width+xx);
            const uint16_t pixel=uint16_t(p[0])|(uint16_t(p[1])<<8);
            if(pixel!=0xf81f) out[(y+yy)*320+x+xx]=pixel;
        }
    return true;
}
void load_background() {
    size_t n=0;
    have_bg=storage::read_file("/system/assets/nes-background.565",custom_bg,sizeof(custom_bg),n) && n==sizeof(custom_bg);
    if(!have_bg) have_bg=storage::read_file("/rom/nes-background.565",custom_bg,sizeof(custom_bg),n) && n==sizeof(custom_bg);
}
void service(uint32_t now_ms) {
    if(!battery_sampled || now_ms-last_battery_sample>=30000) {
        battery::refresh(now_ms); last_battery_sample=now_ms; battery_sampled=true;
    }
}
uint16_t blend(uint16_t a,uint16_t b,int alpha) {
    int r=((a>>11)*(255-alpha)+(b>>11)*alpha)/255;
    int g=(((a>>5)&63)*(255-alpha)+((b>>5)&63)*alpha)/255;
    int bl=((a&31)*(255-alpha)+(b&31)*alpha)/255;
    return (r<<11)|(g<<5)|bl;
}
void box(uint16_t *out,int x,int y,int w,int h,uint16_t c) {
    for(int yy=std::max(0,y);yy<std::min(240,y+h);++yy)
        for(int xx=std::max(0,x);xx<std::min(320,x+w);++xx) out[yy*320+xx]=c;
}
void circle(uint16_t *out,int cx,int cy,int radius,uint16_t color) {
    for(int y=-radius;y<=radius;++y) for(int x=-radius;x<=radius;++x)
        if(x*x+y*y<=radius*radius && cx+x>=0 && cx+x<320 && cy+y>=0 && cy+y<240)
            out[(cy+y)*320+cx+x]=color;
}
int glyph_width(const char *s,const art::Glyph *glyphs) {
    int width=0;
    for(;*s;++s) {
        unsigned c=static_cast<unsigned char>(*s);
        if((c&0xc0)==0x80) continue;
        width+=glyphs[(c>=32 && c<127?c:'?')-32].advance;
    }
    return width;
}
void glyph_text(uint16_t *out,const char *s,int x,int y,uint16_t color,
                 const art::Glyph *glyphs,const uint8_t *pixels,
                 int clip_top,int clip_bottom) {
    for(;*s;++s) {
        unsigned c=static_cast<unsigned char>(*s);
        if((c&0xc0)==0x80) continue;
        auto &g=glyphs[(c>=32 && c<127?c:'?')-32];
        for(int yy=0;yy<g.height;++yy) {
            if(y+yy<std::max(0,clip_top) || y+yy>=std::min(240,clip_bottom)) continue;
            for(int xx=0;xx<g.width;++xx) if(x+xx>=0 && x+xx<320) {
                uint8_t a=pixels[g.offset+yy*g.width+xx];
                if(a) out[(y+yy)*320+x+xx]=blend(out[(y+yy)*320+x+xx],color,a);
            }
        }
        x+=g.advance;
    }
}
void status_icons(uint16_t *out,BatteryStyle style) {
    const auto &reading=battery::reading();
    const uint16_t grey=rgb(100,100,100);
    const int level=std::min(100,int(reading.percent));
    uint16_t color=!reading.valid?grey:level<50?rgb(225+21*level/50,64+120*level/50,75-25*level/50):
        rgb(246-246*(level-50)/50,184-18*(level-50)/50,50+56*(level-50)/50);
    if(style==BatteryStyle::Settings)
        color=!reading.valid?rgb(81,79,71):level<20?rgb(158,43,31):rgb(36,80,57);
    constexpr int top=status_center_y-6;
    box(out,270,top,23,1,color); box(out,270,top+11,23,1,color); box(out,270,top,1,12,color); box(out,292,top,1,12,color);
    box(out,293,top+4,2,4,color);
    if(reading.valid) {
        const int width=17*level/100;
        if(width) box(out,273,top+3,width,6,color);
    }
    uint16_t connection=grey;
    const auto active=input::hub.active_remote();
    if(active==input::Source::Ble && input::hub.status(input::Source::Ble).available) connection=rgb(70,220,100);
    else if(active==input::Source::Serial && input::hub.status(input::Source::Serial).available) connection=rgb(65,145,255);
    circle(out,305,status_center_y,4,connection);
}
void canvas(uint16_t *out) {
    if(have_bg) std::memcpy(out,custom_bg,sizeof(custom_bg));
    else std::fill(out,out+320*240,0);
    glyph_text(out,"TUFTY",14,8,white,art::header_glyphs,art::header_pixels);
    box(out,80,9,1,23,rgb(78,78,78));
    glyph_text(out,"NES",92,8,cyan,art::header_glyphs,art::header_pixels);
    status_icons(out);
}
void rounded_box(uint16_t *out,int x,int y,int w,int h,int radius,uint16_t color) {
    if(w<=0 || h<=0) return;
    radius=std::clamp(radius,0,std::min(w,h)/2);
    for(int row=0;row<h;++row) {
        const int dy=row<radius?radius-row-1:row>=h-radius?row-(h-radius):0;
        int inset=0;
        while(inset<radius && (radius-inset-1)*(radius-inset-1)+dy*dy>radius*radius) ++inset;
        box(out,x+inset,y+row,w-2*inset,1,color);
    }
}
int width(const char *s,Font font) {
    const auto *glyphs=font==Font::Title?art::title_glyphs:art::body_glyphs;
    int advance=0,bounds=0;
    for(;*s;++s) {
        unsigned c=uint8_t(*s); if((c&0xc0)==0x80) continue;
        const auto &g=glyphs[(c>=32 && c<127?c:'?')-32];
        bounds=std::max(bounds,advance+int(g.width)); advance+=g.advance;
    }
    return std::max(bounds,advance);
}
void text(uint16_t *out,const char *s,int x,int y,uint16_t color,int max_width,Font font) {
    const auto *glyphs=font==Font::Title?art::title_glyphs:art::body_glyphs;
    const auto *pixels=font==Font::Title?art::title_pixels:art::body_pixels;
    char clipped[128]{}; unsigned used=0; int advance=0;
    for(;*s && used+1<sizeof(clipped);++s) {
        unsigned c=uint8_t(*s); if((c&0xc0)==0x80) continue;
        c=c>=32 && c<127?c:'?'; const auto &g=glyphs[c-32];
        if(advance+std::max(int(g.advance),int(g.width))>max_width) break;
        clipped[used++]=char(c); advance+=g.advance;
    }
    glyph_text(out,clipped,x,y,color,glyphs,pixels);
}
void text_centered(uint16_t *out,const char *s,int x,int top,int height,uint16_t color,int max_width) {
    // Font bitmaps include 19 rows, but most visible letters occupy only
    // 11–14. Cache each glyph's ink bounds once rather than scanning pixels
    // every time a row is drawn.
    struct Bounds {uint8_t top,bottom;};
    static Bounds bounds[95]{};
    static bool ready=false;
    if(!ready) {
        for(unsigned i=0;i<95;++i) {
            const auto &g=art::body_glyphs[i]; bounds[i]={g.height,0};
            for(unsigned y=0;y<g.height;++y) for(unsigned xx=0;xx<g.width;++xx)
                if(art::body_pixels[g.offset+y*g.width+xx]) {
                    bounds[i].top=std::min(bounds[i].top,uint8_t(y));
                    bounds[i].bottom=uint8_t(y+1);
                }
        }
        ready=true;
    }
    int ink_top=255,ink_bottom=0,advance=0;
    unsigned used=0;
    for(const char *p=s;*p && used<127;++p) {
        unsigned c=uint8_t(*p); if((c&0xc0)==0x80) continue;
        c=c>=32 && c<127?c:'?'; const auto &g=art::body_glyphs[c-32];
        if(advance+std::max(int(g.advance),int(g.width))>max_width) break;
        const auto b=bounds[c-32];
        if(b.bottom) {ink_top=std::min(ink_top,int(b.top)); ink_bottom=std::max(ink_bottom,int(b.bottom));}
        advance+=g.advance; ++used;
    }
    if(ink_bottom) text(out,s,x,top+(height-(ink_bottom-ink_top))/2-ink_top,color,max_width);
}
}
