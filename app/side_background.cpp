#include "side_background.hpp"
#include "configuration.hpp"
#include "presentation/palette_sample.hpp"
#include <algorithm>
#include <memory>
#include <new>
#ifndef TUFTY_HOST_PREVIEW
#include "pico/stdlib.h"
#else
#define __uninitialized_psram(name)
#endif
namespace side_background {
namespace {
constexpr unsigned capacity=64;
struct Entry {char path[path_bytes];};
std::unique_ptr<Entry[]> entries;
unsigned size=0,index=0,width=0;
bool truncated=false,scan_error=false,previewing=false;
presentation::Position position=presentation::Position::Center;
uint16_t __uninitialized_psram("side_background") pixels[64*240];
const char *path(unsigned i) {return i && i<=size?entries[i-1].path:"";}
const char *leaf(const char *value) {return std::strrchr(value,'/')+1;}
void found(const char *name,uint32_t bytes,void *context) {
    if(!valid_size(position,bytes)) return;
    Entry entry;
    const int n=std::snprintf(entry.path,sizeof entry.path,"%s/%s",static_cast<const char *>(context),name);
    if(n<0 || size_t(n)>=sizeof entry.path || !valid_path(position,entry.path)) return;
    unsigned at=0;
    while(at<size) {
        const int order=strcasecmp(leaf(entries[at].path),name);
        if(order>0 || (!order && std::strcmp(entries[at].path,entry.path)>=0)) break;
        ++at;
    }
    if(at<size && !std::strcmp(entries[at].path,entry.path)) return;
    if(size==capacity) {truncated=true;if(at==capacity) return;} else ++size;
    for(unsigned i=size-1;i>at;--i) entries[i]=entries[i-1];
    entries[at]=entry;
}
bool load(const char *path) {
    width=0;
    if(!*path) return true;
    if(!valid_path(position,path)) return false;
    // The temporary source is freed before the emulator allocates its working RAM.
    std::unique_ptr<uint16_t[]> source(new(std::nothrow) uint16_t[64*240]);
    if(!source) return false;
    size_t bytes=0;
    if(!storage::read_file(path,source.get(),64*240*2,bytes) || !valid_size(position,bytes)) return false;
    width=unsigned(bytes/(240*2));
    for(unsigned x=0;x<width;++x) for(unsigned y=0;y<240;++y) pixels[x*240+y]=source[y*width+x];
    return true;
}
}
bool open() {
    close();position=storage::config.game_position;
    entries.reset(new(std::nothrow) Entry[capacity]);if(!entries) return false;
    char rom[64],usb[64];directory(rom,sizeof rom,false,position);directory(usb,sizeof usb,true,position);
    const bool a=storage::visit_files(rom,found,rom),b=storage::visit_files(usb,found,usb);
    scan_error=!a || !b;
    for(unsigned i=0;i<count();++i) if(selected(i)) {index=i;break;}
    return true;
}
void close() {entries.reset();size=index=0;truncated=scan_error=previewing=false;}
unsigned count() {return size+1;}
unsigned current() {return index;}
bool limited() {return truncated;}
bool incomplete() {return scan_error;}
const char *name(unsigned i) {return i && i<=size?leaf(path(i)):"Black";}
const char *source(unsigned i) {return i && i<=size?(!std::strncmp(path(i),"/rom/",5)?"ROM":"USB"):"";}
bool selected(unsigned i) {return i<=size && !std::strcmp(path(i),storage::config.side_backgrounds[unsigned(position)]);}
bool preview(unsigned i) {
    previewing=false;if(i>size || !load(path(i))) return false;
    index=i;previewing=true;return true;
}
void apply() {
    if(!previewing) return;
    std::snprintf(storage::config.side_backgrounds[unsigned(position)],path_bytes,"%s",path(index));
    configuration::sync();
}
presentation::GameView view() {return {position,width?pixels:nullptr,width,storage::config.fps_monitor && position!=presentation::Position::Center};}
void prepare() {
    close();position=storage::config.game_position;
    load(storage::config.side_backgrounds[unsigned(position)]); // Missing/invalid selection falls back to black.
}
void draw_preview(uint16_t *out) {
    const auto v=view();const unsigned left=presentation::game_x(position);
    presentation::FpsMonitor monitor;monitor.reset(position,v.fps);
    for(unsigned x=0;x<320;++x) {
        if(x>=left && x<left+256) continue;
        uint16_t column[240];const auto *image=v.column(x);
        for(unsigned y=0;y<240;++y) column[y]=image?image[y]:0;
        monitor.column(x,column);
        for(unsigned y=0;y<240;++y) out[y*320+x]=column[y];
    }
    const auto *colors=presentation::palette_colors(storage::config.palette);
    presentation::draw_palette_sample(out,left,0,colors);
}
}
