#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace bluetooth {
inline int hex(char c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
inline bool address(const char *text,uint8_t out[6]) {
    if(!text || std::strlen(text)!=17) return false;
    for(unsigned i=0;i<6;++i) {
        int a=hex(text[i*3]),b=hex(text[i*3+1]);
        if(a<0 || b<0 || (i<5 && text[i*3+2]!=':')) return false;
        out[i]=uint8_t(a*16+b);
    }
    return true;
}
inline bool uuid16(const char *text,uint16_t &out) {
    if(!text || std::strlen(text)!=6 || text[0]!='0' || (text[1]!='x' && text[1]!='X')) return false;
    unsigned value=0;
    for(unsigned i=2;i<6;++i) {int n=hex(text[i]); if(n<0) return false; value=(value<<4)|unsigned(n);}
    if(!value) return false;
    out=uint16_t(value); return true;
}
struct Peer {
    uint8_t address[6]{};
    bool random=false;
    bool operator==(const Peer &other) const {
        return random==other.random && !std::memcmp(address,other.address,6);
    }
    void text(char out[18]) const {
        std::snprintf(out,18,"%02X:%02X:%02X:%02X:%02X:%02X",address[0],address[1],address[2],address[3],address[4],address[5]);
    }
};
enum class Mode : uint8_t { AutoCompatible,WhitelistOnly };
struct Policy {
    static constexpr size_t Capacity=4;
    Mode mode=Mode::AutoCompatible;
    Peer peers[Capacity]{};
    uint8_t count=0,preferred=255;
    int find(const Peer &peer) const {
        for(unsigned i=0;i<count;++i) if(peers[i]==peer) return int(i);
        return -1;
    }
    bool add(const Peer &peer) {
        if(find(peer)>=0) return true;
        if(count>=Capacity) return false;
        peers[count++]=peer; return true;
    }
    bool remove(const Peer &peer) {
        int at=find(peer); if(at<0) return false;
        for(unsigned i=unsigned(at);i+1<count;++i) peers[i]=peers[i+1];
        --count; peers[count]={};
        if(preferred==at) preferred=255;
        else if(preferred!=255 && preferred>at) --preferred;
        return true;
    }
};
}
