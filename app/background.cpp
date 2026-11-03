#include "background.hpp"
#include "configuration.hpp"
#include "ui.hpp"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>

namespace background {
namespace {
constexpr unsigned capacity=64;
struct Entry {char path[path_bytes];};
std::unique_ptr<Entry[]> entries;
unsigned size=0,view=0;
bool truncated=false,previewing=false,scan_error=false;
const char *leaf(const char *path) {return std::strrchr(path,'/')+1;}
int compare(const char *a,const char *b) {
    const int order=strcasecmp(leaf(a),leaf(b));
    return order?order:std::strcmp(a,b);
}
void found(const char *name,uint32_t bytes,void *context) {
    if(bytes!=320*240*2) return;
    Entry entry;
    const int n=std::snprintf(entry.path,sizeof entry.path,"%s/%s",static_cast<const char *>(context),name);
    if(n<0 || size_t(n)>=sizeof entry.path || !valid_path(entry.path)) return;
    unsigned at=0;
    while(at<size && compare(entries[at].path,entry.path)<0) ++at;
    if(at<size && !std::strcmp(entries[at].path,entry.path)) return;
    if(size==capacity) {truncated=true; if(at==capacity) return;}
    else ++size;
    for(unsigned i=size-1;i>at;--i) entries[i]=entries[i-1];
    entries[at]=entry;
}
}
bool open() {
    close(); entries.reset(new(std::nothrow) Entry[capacity]);
    if(!entries) return false;
    const bool rom_ok=storage::visit_files(rom_directory,found,const_cast<char *>(rom_directory));
    const bool system_ok=storage::visit_files(system_directory,found,const_cast<char *>(system_directory));
    if(!rom_ok && !system_ok) {close(); return false;}
    scan_error=!rom_ok || !system_ok;
    for(unsigned i=0;i<size;++i) if(selected(i)) {view=i; break;}
    return true;
}
void restore() {if(previewing) {ui::load_background(); previewing=false;}}
void close() {restore(); entries.reset(); size=view=0; truncated=scan_error=false;}
unsigned count() {return size;}
bool limited() {return truncated;}
bool incomplete() {return scan_error;}
const char *path(unsigned index) {return index<size?entries[index].path:"";}
const char *name(unsigned index) {return index<size?leaf(path(index)):"";}
const char *source(unsigned index) {return index<size && !std::strncmp(path(index),"/rom/",5)?"ROM":"USB";}
bool selected(unsigned index) {
    const auto *saved=storage::config.menu_background;
    return index<size && !std::strcmp(path(index),*saved?saved:default_path);
}
unsigned current() {return view;}
bool preview(unsigned index) {
    if(index>=size) return false;
    if(!ui::preview_background(path(index))) {
        ui::load_background(); previewing=false; return false;
    }
    view=index; previewing=true; return true;
}
bool apply() {
    if(!previewing || view>=size) return false;
    const auto *chosen=path(view);
    std::snprintf(storage::config.menu_background,sizeof storage::config.menu_background,"%s",
        std::strcmp(chosen,default_path)?chosen:"");
    configuration::sync(); return true;
}
}
