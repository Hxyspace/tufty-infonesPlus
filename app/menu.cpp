#include "menu.hpp"
#include "ui.hpp"
#include "storage.hpp"
#include "menu_assets.hpp"
#include "menu_layout.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifndef TUFTY_HOST_PREVIEW
#include "pico/stdlib.h"
#else
#define __uninitialized_psram(name)
#endif

void tufty_service();

namespace menu {
static uint16_t __uninitialized_psram("cover") covers[3][58*80];
struct Cover { int index=-1; uint32_t generation=0; bool valid=false; };
static Cover cover_cache[3];
static bool prefetch_wait=true;
static uint32_t prefetch_since=0;
static int cover_index=-1;
static uint32_t cover_generation=0;
static int description_index=-1;
static uint32_t description_generation=0;
static int description_page=0;
static uint32_t __uninitialized_psram("description_pages") checkpoints[layout::MaxCheckpoints];
static unsigned description_pages=1;
static const char *description_source="No description available";
static char body_lines[layout::LinesPerPage][layout::LineBytes];
static char page_label[24]{};
struct PreviousCard {
    int index=-1;
    uint32_t generation=0;
    unsigned page=0,pages=1;
    char lines[layout::LinesPerPage][layout::LineBytes];
    char label[24];
};
static PreviousCard previous;
static int cached_cover(int index) {
    for(int slot=0;slot<3;++slot)
        if(cover_cache[slot].index==index && cover_cache[slot].generation==storage::generation()) return slot;
    return -1;
}
static void load_cover(int index) {
    int slot=-1;
    for(int i=0;i<3;++i) {
        const auto &c=cover_cache[i];
        if(c.index<0 || c.generation!=storage::generation() || std::abs(c.index-cover_index)>1) {slot=i; break;}
    }
    if(slot<0) return;
    const auto generation=storage::generation();
    auto &entry=cover_cache[slot]; entry={index,generation,false};
    if(!storage::has_cover(index)) return;
    char path[280]; std::snprintf(path,sizeof(path),"/system/roms/%s",storage::entries[index].name);
    char *dot=std::strrchr(path,'.'); if(!dot) return;
    std::strcpy(dot,".565"); size_t n=0;
    const bool loaded=storage::read_file(path,covers[slot],sizeof(covers[slot]),n) && n==sizeof(covers[slot]);
    entry.valid=loaded && generation==storage::generation();
}
// Only call after presenting the selection (with a placeholder on a miss).
bool load_current_cover() {
    if(cover_index<0 || size_t(cover_index)>=storage::count ||
       cover_generation!=storage::generation() || cached_cover(cover_index)>=0) return false;
    load_cover(cover_index);
    return true;
}
// Called after presenting a frame. No queue: every step uses the latest selection.
bool prefetch(uint32_t now_ms,bool navigation_active) {
    if(prefetch_wait || navigation_active) {prefetch_since=now_ms; prefetch_wait=false; return false;}
    if(now_ms-prefetch_since<50 || cover_index<0 || cover_generation!=storage::generation()) return false;
    if(cached_cover(cover_index)<0) return false; // Current selection always takes priority.
    for(int offset : {-1,1}) {
        const int index=cover_index+offset;
        if(index>=0 && size_t(index)<storage::count && cached_cover(index)<0) {
            load_cover(index); ++timing.prefetches; return true;
        }
    }
    return false;
}

using ui::rgb;
using ui::blend;
using ui::box;
using ui::circle;
static int text_width(const char *s,const art::Glyph *glyphs) {return ui::glyph_width(s,glyphs);}
static void text(uint16_t *out,const char *s,int x,int y,uint16_t color,
                 const art::Glyph *glyphs=art::body_glyphs,const uint8_t *pixels=art::body_pixels,
                 int clip_top=0,int clip_bottom=240) {
    ui::glyph_text(out,s,x,y,color,glyphs,pixels,clip_top,clip_bottom);
}
static void title(uint16_t *out,const char *source,int x,int y) {
    char line[128]{}; size_t used=0; int row=0, width=0;
    for(const char *p=source;*p && row<2;++p) {
        unsigned c=static_cast<unsigned char>(*p);
        if((c&0xc0)==0x80) continue;
        if(c<32 || c>=127) c='?';
        const int advance=art::title_glyphs[c-32].advance;
        if(width+advance>194 && used) {
            line[used]=0; text(out,line,x,y+row*29,0xffff,art::title_glyphs,art::title_pixels,40,210);
            ++row; used=0; width=0;
        }
        if(row>=2) break;
        if(used+1<sizeof(line)) {line[used++]=static_cast<char>(c); width+=advance;}
    }
    if(row<2 && used) {line[used]=0; text(out,line,x,y+row*29,0xffff,art::title_glyphs,art::title_pixels,40,210);}
}
static const char *fallback_title(size_t index) {
    static char name[256];
    if(index>=storage::count) return "";
    std::snprintf(name,sizeof(name),"%s",storage::entries[index].name);
    char *dot=std::strrchr(name,'.'); if(dot) *dot=0;
    for(char *p=name;*p;++p) if(*p=='_') *p=' ';
    return name;
}
static const char *display_title(size_t index) {
    const char *value=storage::title(index);
    return value && *value ? value : fallback_title(index);
}
static void prepare_body_lines() {
    const unsigned first=unsigned(description_page)*layout::LinesPerPage;
    const unsigned checkpoint=first/layout::CheckpointLines;
    uint32_t position=checkpoints[checkpoint];
    for(unsigned line=checkpoint*layout::CheckpointLines;line<first;++line)
        position=layout::wrap(description_source,position,art::body_glyphs).next;
    for(unsigned row=0;row<layout::LinesPerPage;++row) {
        const auto line=layout::wrap(description_source,position,art::body_glyphs);
        const size_t n=std::min(size_t(line.end-line.start),sizeof(body_lines[row])-1);
        std::memcpy(body_lines[row],description_source+line.start,n); body_lines[row][n]=0;
        position=line.next;
    }
    std::snprintf(page_label,sizeof(page_label),"%u/%u",unsigned(description_page)+1,description_pages);
}
static void prepare_description(int index) {
    if(index==description_index && description_generation==storage::generation()) return;
    description_index=index; description_generation=storage::generation(); description_page=0;
    description_source=storage::description(index);
    if(!description_source || !*description_source) description_source="No description available";
    unsigned lines=0;
    uint32_t position=0;
    checkpoints[0]=0;
    while(description_source[position]) {
        if(lines%layout::CheckpointLines==0) {
            checkpoints[lines/layout::CheckpointLines]=position;
            tufty_service();
        }
        position=layout::wrap(description_source,position,art::body_glyphs).next;
        ++lines;
    }
    description_pages=std::max(1u,(lines+layout::LinesPerPage-1)/layout::LinesPerPage);
    prepare_body_lines();
}
void load_art() {
    ui::load_background();
    ui::load_icons();
    cover_index=-1; description_index=-1; previous.index=-1;
    for(auto &entry:cover_cache) entry={};
    prefetch_wait=true;
}
void enter(uint32_t now_ms) {ui::service(now_ms);}
void service(uint32_t now_ms) {ui::service(now_ms);}
void select(int index) {
    const auto generation=storage::generation();
    if(index!=description_index && description_index>=0 && description_generation==generation) {
        previous.index=description_index; previous.generation=generation;
        previous.page=description_page; previous.pages=description_pages;
        std::memcpy(previous.lines,body_lines,sizeof(body_lines));
        std::memcpy(previous.label,page_label,sizeof(page_label));
    }
    if(index>=0 && static_cast<size_t>(index)<storage::count) prepare_description(index);
    if(index==cover_index && cover_generation==storage::generation()) return;
    cover_index=index; cover_generation=generation; prefetch_wait=true;
    if(index<0 || static_cast<size_t>(index)>=storage::count) return;
    if(cached_cover(index)>=0) {++timing.cover_hits; return;}
    ++timing.cover_misses; // Read only after the placeholder frame has been presented.
}
void reset_page() {description_page=0; description_index=-1;}
void previous_page() {if(description_index>=0 && description_page>0) {--description_page; prepare_body_lines();}}
void next_page() {if(description_index>=0 && description_page+1<static_cast<int>(description_pages)) {++description_page; prepare_body_lines();}}
void draw(uint16_t *out,int selected,float position,uint32_t elapsed,const char *message) {
    ui::canvas(out);
    {
        const int first=std::max(0,static_cast<int>(std::floor(position))-1);
        const int last=std::min<int>(storage::count+1,first+4);
        for(int card=first;card<last;++card) {
            const int i=card-1;
            const int left=13+std::lround((card-position)*310);
            if(left>=320 || left+294<=0) continue;
            const int iy=66+std::lround(std::sin((elapsed+card*700)/2400.0f*6.2831853f)*3);
            if(card==0) {
                if(!ui::icon(out,ui::Icon::Settings,left+11,iy+11)) {
                    const auto cyan=rgb(0,193,222);
                    for(int tooth=0;tooth<8;++tooth) {
                        const float angle=tooth*0.78539816f;
                        box(out,left+35+std::lround(std::cos(angle)*21),iy+35+std::lround(std::sin(angle)*21),10,10,cyan);
                    }
                    circle(out,left+40,iy+40,23,cyan);
                    circle(out,left+40,iy+40,14,rgb(0,43,73));
                    circle(out,left+40,iy+40,6,rgb(255,174,45));
                }
                title(out,"Settings",left+94,53);
                text(out,"SYSTEM",left+40-text_width("SYSTEM",art::body_glyphs)/2,153,rgb(115,120,128));
                const char *lines[]={"Display and controls","Power and device","preferences.","No games installed."};
                for(int row=0;row<(storage::count?3:4);++row)
                    text(out,lines[row],left+94,119+22*row,rgb(192,197,203),art::body_glyphs,art::body_pixels,40,210);
                continue;
            }
            const bool old=previous.index==i && previous.generation==storage::generation();
            const int slot=cached_cover(i);
            const uint16_t *pixels=slot>=0 && cover_cache[slot].valid?covers[slot]:nullptr;
            if(pixels) {
                for(int y=0;y<80;++y) for(int x=0;x<58;++x)
                    if(left+11+x>=0 && left+11+x<320) out[(iy+y)*320+left+11+x]=pixels[y*58+x];
            } else {
                box(out,left+14,iy,54,67,rgb(0,193,222)); box(out,left+19,iy+5,44,32,rgb(0,43,73));
                box(out,left+25,iy+46,9,12,rgb(255,174,45)); box(out,left+45,iy+46,9,12,rgb(255,174,45));
            }
            title(out,display_title(i),left+94,53);
            const bool current=i==description_index && description_generation==storage::generation();
            const auto *lines=current?body_lines:old?previous.lines:nullptr;
            const unsigned pages=current?description_pages:old?previous.pages:1;
            const unsigned page=current?unsigned(description_page):old?previous.page:0;
            const char *label=current?page_label:old?previous.label:"";
            const uint16_t auxiliary=rgb(115,120,128);
            char counter[16]; std::snprintf(counter,sizeof(counter),"%02u/%02u",unsigned(i)+1,unsigned(storage::count));
            // The left column has 94 px; use its full width for large counts.
            text(out,counter,left+40-text_width(counter,art::body_glyphs)/2,153,auxiliary,art::body_glyphs,art::body_pixels,40,210);
            if(card==selected && message) {
                text(out,message,left+94,119,rgb(255,174,100),art::body_glyphs,art::body_pixels,40,210);
            } else if(lines) {
                for(unsigned row=0;row<4;++row) text(out,lines[row],left+94,119+row*22,rgb(192,197,203),art::body_glyphs,art::body_pixels,40,210);
            }
            if(pages>1) {
                text(out,label,left+40-text_width(label,art::body_glyphs)/2,184,auxiliary,art::body_glyphs,art::body_pixels,40,210);
                for(int row=0;row<4;++row) {
                    box(out,left+40-row,175+row,row*2+1,1,page?auxiliary:rgb(50,53,58));
                    box(out,left+37+row,207+row,7-row*2,1,page+1<pages?auxiliary:rgb(50,53,58));
                }
            }
        }
    }
    const uint16_t orange=rgb(255,174,45), dim=rgb(81,44,14);
    for(int y=-5;y<=5;++y) {
        box(out,51+std::abs(y),225+y,2,1,selected>0?orange:dim);
        box(out,258-std::abs(y),225+y,2,1,selected<static_cast<int>(storage::count)?orange:dim);
    }
    box(out,80,225,160,2,dim);
    const int thumb=storage::count?std::lround(position*144/storage::count):0;
    box(out,80+thumb,223,16,5,orange);
    if(elapsed<250) for(int i=0;i<320*240;++i) out[i]=blend(0,out[i],elapsed*255/250);
}
}
