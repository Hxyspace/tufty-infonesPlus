#include "runtime.hpp"
#include "layout.hpp"
#include <cstring>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace preview {
namespace {
using namespace tufty::storage::layout;
constexpr size_t capacity=16*1024*1024;
uint8_t *mapping=nullptr;
std::filesystem::path state_file;
bool dirty=false;
void load(const std::filesystem::path &path,uint32_t offset,size_t bytes) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if(!in || in.tellg()!=static_cast<std::streamoff>(bytes))
        throw std::runtime_error("Missing or wrong-sized image: "+path.string());
    in.seekg(0); in.read(reinterpret_cast<char *>(mapping+offset),bytes);
    if(!in) throw std::runtime_error("Cannot read "+path.string());
}
}
bool fail_save=false;
void mount_flash(const std::filesystem::path &images) {
    void *address=reinterpret_cast<void *>(kXipBase);
#ifdef _WIN32
    mapping=static_cast<uint8_t *>(VirtualAlloc(address,capacity,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
#else
    void *allocated=mmap(address,capacity,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    mapping=allocated==MAP_FAILED?nullptr:static_cast<uint8_t *>(allocated);
#endif
    if(mapping!=address) {unmount_flash(); throw std::runtime_error("Cannot reserve host flash address 0x10000000");}
    std::memset(mapping,255,capacity);
    load(images/"rom.bin",kRom.offset,kRom.bytes);
    load(images/"system.bin",kSystem.offset,kSystem.bytes);
    state_file=images/"state.bin";
    load(state_file,kState.offset,kState.bytes);
}
void flush_state() {
    if(!mapping || !dirty) return;
    auto temporary=state_file; temporary+=".tmp";
    {
        std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
        out.write(reinterpret_cast<const char *>(mapping+kState.offset),kState.bytes);
        out.close();
        if(!out) throw std::runtime_error("Cannot save host State: "+temporary.string());
    }
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),state_file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace host State");
#else
    std::filesystem::rename(temporary,state_file);
#endif
    dirty=false;
}
void unmount_flash() {
    if(!mapping) return;
#ifdef _WIN32
    VirtualFree(mapping,0,MEM_RELEASE);
#else
    munmap(mapping,capacity);
#endif
    mapping=nullptr;
}
void write_flash(uint32_t at,const uint8_t *data,size_t bytes) {
    if(!mapping || at<kState.offset || bytes>kState.bytes || at-kState.offset>kState.bytes-bytes)
        throw std::runtime_error("Host flash write outside State");
    if(data) {
        for(size_t i=0;i<bytes;++i) {
            if((mapping[at+i]&data[i])!=data[i]) throw std::runtime_error("Host flash requires erase before program");
            mapping[at+i]=data[i];
        }
    } else std::memset(mapping+at,255,bytes);
    dirty=true;
}
}
int flash_safe_execute(void (*fn)(void *),void *arg,unsigned) {
    if(preview::fail_save) return -1;
    fn(arg); return 0;
}
void flash_range_erase(uint32_t at,size_t bytes) {preview::write_flash(at,nullptr,bytes);}
void flash_range_program(uint32_t at,const uint8_t *data,size_t bytes) {preview::write_flash(at,data,bytes);}
