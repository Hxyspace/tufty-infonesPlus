#pragma once
#include <cstddef>
#include <cstring>
#include <strings.h>
#include <initializer_list>

namespace background {
constexpr size_t path_bytes=288;
constexpr const char *rom_directory="/rom/background";
constexpr const char *system_directory="/system/assets/background";
constexpr const char *default_path="/rom/background/background.565";
inline bool valid_path(const char *path) {
    if(!path || std::strlen(path)>=path_bytes) return false;
    const char *leaf=nullptr;
    for(const char *directory:{rom_directory,system_directory}) {
        const size_t n=std::strlen(directory);
        if(!std::strncmp(path,directory,n) && path[n]=='/') leaf=path+n+1;
    }
    if(!leaf) return false;
    const size_t n=std::strlen(leaf);
    if(n<=4 || n>255 || strcasecmp(leaf+n-4,".565") || std::strstr(leaf,"..")) return false;
    for(const unsigned char *p=reinterpret_cast<const unsigned char *>(leaf);*p;++p)
        if(*p<32 || *p==127 || *p=='/' || *p=='\\' || *p==':') return false;
    return true;
}
}
