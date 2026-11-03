#pragma once
#include "presentation/game_view.hpp"
#include <cstdio>
#include <cstring>
#include <strings.h>
namespace side_background {
constexpr unsigned path_bytes=128;
inline void directory(char *out,size_t bytes,bool usb,presentation::Position position) {
    std::snprintf(out,bytes,"%s/side-background/%s",usb?"/system/assets":"/rom",position==presentation::Position::Center?"center":"side");
}
inline bool valid_path(presentation::Position position,const char *path) {
    if(!path || std::strlen(path)>=path_bytes) return false;
    for(bool usb:{false,true}) {
        char root[64];directory(root,sizeof root,usb,position);
        const size_t prefix=std::strlen(root);
        if(std::strncmp(path,root,prefix) || path[prefix]!='/') continue;
        const char *name=path+prefix+1;const size_t size=std::strlen(name);
        if(size<=4 || size>80 || strcasecmp(name+size-4,".565") || std::strstr(name,"..")) return false;
        for(const unsigned char *p=reinterpret_cast<const unsigned char *>(name);*p;++p)
            if(*p<32 || *p==127 || *p=='/' || *p=='\\' || *p==':') return false;
        return true;
    }
    return false;
}
inline bool valid_size(presentation::Position position,size_t bytes) {
    return bytes==64*240*2 || (position==presentation::Position::Center && bytes==32*240*2);
}
}
