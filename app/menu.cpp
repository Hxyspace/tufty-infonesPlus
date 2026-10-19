#include "menu.hpp"
#include "storage.hpp"
#include "menu_assets.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifndef TUFTY_HOST_PREVIEW
#include "pico/stdlib.h"
#else
#define __uninitialized_psram(name)
#endif
namespace menu {
static uint16_t __uninitialized_psram("art") custom_bg[320*200];
static uint16_t __uninitialized_psram("cover") custom_cover[60*72];
static bool have_bg=false, have_cover=false;
static int cover_index=-1;
static uint16_t rgb(int r,int g,int b) {return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static uint16_t blend(uint16_t a,uint16_t b,int alpha) {
    int r=((a>>11)*(255-alpha)+(b>>11)*alpha)/255;
    int g=(((a>>5)&63)*(255-alpha)+((b>>5)&63)*alpha)/255;
    int bl=((a&31)*(255-alpha)+(b&31)*alpha)/255;
    return (r<<11)|(g<<5)|bl;
}
static void box(uint16_t *out,int x,int y,int w,int h,uint16_t c) {
    for(int yy=std::max(0,y);yy<std::min(240,y+h);++yy)
        for(int xx=std::max(0,x);xx<std::min(320,x+w);++xx) out[yy*320+xx]=c;
}
static int text_width(const char *s,const art::Glyph *glyphs) {
    int width=0;
    for(;*s;++s) {
        unsigned c=static_cast<unsigned char>(*s);
        if((c&0xc0)==0x80) continue;
        width+=glyphs[(c>=32 && c<127?c:'?')-32].advance;
    }
    return width;
}
static void text(uint16_t *out,const char *s,int x,int y,uint16_t color,
                 const art::Glyph *glyphs=art::body_glyphs,const uint8_t *pixels=art::body_pixels,
                 int clip_top=0,int clip_bottom=240) {
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
static void title(uint16_t *out,const char *source,int x,int y) {
    char line[128]{}; size_t used=0; int row=0, width=0;
    for(const char *p=source;*p && row<2;++p) {
        unsigned c=static_cast<unsigned char>(*p);
        if((c&0xc0)==0x80) continue;
        if(c<32 || c>=127) c='?';
        int advance=art::title_glyphs[c-32].advance;
        if(width+advance>194 || used>=sizeof(line)-2) {
            line[used]=0; text(out,line,x,y+row*29,0xffff,art::title_glyphs,art::title_pixels,40,194);
            ++row; used=0; width=0;
        }
        line[used++]=c; width+=advance;
    }
    if(row<2 && used) {line[used]=0; text(out,line,x,y+row*29,0xffff,art::title_glyphs,art::title_pixels,40,194);}
}
void load_art() {
    size_t n=0;
    have_bg=storage::read_file("/system/assets/nes-background.rgb565",custom_bg,sizeof(custom_bg),n) && n==sizeof(custom_bg);
    if(!have_bg) have_bg=storage::read_file("/rom/nes-background.rgb565",custom_bg,sizeof(custom_bg),n) && n==sizeof(custom_bg);
}
void cover(int index) {
    if(index==cover_index) return;
    cover_index=index; have_cover=false;
    if(index<0 || static_cast<size_t>(index)>=storage::count) return;
    char path[280]; std::snprintf(path,sizeof(path),"/system/roms/%s",storage::entries[index].name);
    char *dot=std::strrchr(path,'.'); if(!dot) return;
    std::strcpy(dot,".rgb565"); size_t n=0;
    have_cover=storage::read_file(path,custom_cover,sizeof(custom_cover),n) && n==sizeof(custom_cover);
}
void draw(uint16_t *out,int selected,float position,uint32_t elapsed,const char *message) {
    std::fill(out,out+320*40,0);
    std::memcpy(out+320*40,have_bg?custom_bg:art::background,320*200*2);
    text(out,"TUFTY",14,8,0xffff,art::header_glyphs,art::header_pixels);
    box(out,80,9,1,23,rgb(78,78,78));
    text(out,"NES",92,8,rgb(0,193,222),art::header_glyphs,art::header_pixels);
    text(out,"GAME LIBRARY",220,13,rgb(163,168,174));
    if(storage::count && !message) {
        int first=std::max(0,static_cast<int>(std::floor(position))-1);
        int last=std::min<int>(storage::count,first+4);
        for(int i=first;i<last;++i) {
            int left=13+std::lround((i-position)*310);
            if(left>=320 || left+294<=0) continue;
            int iy=66+std::lround(std::sin((elapsed+i*700)/2400.0f*6.2831853f)*3);
            if(have_cover && i==cover_index) {
                for(int y=0;y<72;++y) for(int x=0;x<60;++x)
                    if(left+11+x>=0 && left+11+x<320) out[(iy+y)*320+left+11+x]=custom_cover[y*60+x];
            } else {
                // Cartridge fallback: game files need no companion artwork.
                box(out,left+14,iy,54,67,rgb(0,193,222));
                box(out,left+19,iy+5,44,32,rgb(0,43,73));
                box(out,left+25,iy+46,9,12,rgb(255,174,45));
                box(out,left+45,iy+46,9,12,rgb(255,174,45));
            }
            char name[256]; std::snprintf(name,sizeof(name),"%s",storage::entries[i].name);
            char *dot=std::strrchr(name,'.'); if(dot) *dot=0;
            for(char *p=name;*p;++p) if(*p=='_') *p=' ';
            title(out,name,left+94,53);
            char details[64]; std::snprintf(details,sizeof(details),"NES cartridge  /  %lu KB",static_cast<unsigned long>(storage::entries[i].bytes/1024));
            text(out,details,left+94,119,rgb(163,168,174),art::body_glyphs,art::body_pixels,40,194);
            text(out,"Home returns to library",left+94,141,rgb(163,168,174),art::body_glyphs,art::body_pixels,40,194);
        }
    } else {
        title(out,storage::count?"Unable to start":"No games yet",24,64);
        text(out,message?message:"Copy .nes files into /system/roms",24,130,rgb(163,168,174));
        text(out,"Console: usb disk",24,153,rgb(163,168,174));
    }
    uint16_t orange=rgb(255,174,45), dim=rgb(81,44,14);
    for(int y=-5;y<=5;++y) {
        box(out,51+std::abs(y),205+y,2,1,selected>0?orange:dim);
        box(out,258-std::abs(y),205+y,2,1,selected+1<static_cast<int>(storage::count)?orange:dim);
    }
    box(out,80,205,160,2,dim);
    int thumb=storage::count>1?std::lround(position*144/(storage::count-1)):0;
    box(out,80+thumb,203,16,5,orange);
    const char *label=storage::count?"PLAY GAME":"ADD GAMES";
    text(out,label,(320-text_width(label,art::body_glyphs))/2,220,orange);
    if(elapsed<250) for(int i=0;i<320*240;++i) out[i]=blend(0,out[i],elapsed*255/250);
}
}
