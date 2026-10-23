// Repeatable real-core ROM smoke test; deliberately independent of SDL/UI.
#include "core_api/host.hpp"
#include "presentation/pixels.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned frames=0,services=0,limit=600,allocations=0,start_frame=600;
uint32_t hash=2166136261u,last_hash=0;
std::array<uint16_t,256*240> screen{};
unsigned positive(const std::string &value) {
    size_t used=0;
    if(value.empty() || value[0]=='-') throw std::runtime_error("Invalid frame count");
    auto n=std::stoul(value,&used);
    if(used!=value.size() || !n || n>1000000) throw std::runtime_error("Frame count must be 1..1000000");
    return unsigned(n);
}
}
namespace infones::port {
void *allocate(size_t n) {auto p=std::malloc(n); if(p) ++allocations; return p;}
void release(void *p) {if(p) --allocations; std::free(p);}
bool has_large_memory() {return true;}
bool remove_sprite_limit() {return false;}
}
int main(int argc,char **argv) {
    try {
        if(argc<2) throw std::runtime_error("Usage: rom_probe ROM [--frames N] [--start-frame N] [--output frame.ppm]");
        std::string output;
        for(int i=2;i<argc;++i) {
            std::string option=argv[i];
            if(++i>=argc) throw std::runtime_error("Missing value for "+option);
            if(option=="--frames") limit=positive(argv[i]);
            else if(option=="--start-frame") start_frame=positive(argv[i]);
            else if(option=="--output") output=argv[i];
            else throw std::runtime_error("Unknown option: "+option);
        }
        std::ifstream in(argv[1],std::ios::binary|std::ios::ate);
        if(!in) throw std::runtime_error("Cannot open ROM");
        const auto size=in.tellg();
        if(size<16 || size>4*1024*1024) throw std::runtime_error("ROM must be 16 bytes..4 MiB");
        in.seekg(0); std::vector<uint8_t> rom(static_cast<size_t>(size));
        if(!in.read(reinterpret_cast<char *>(rom.data()),rom.size())) throw std::runtime_error("Cannot read ROM");
        const auto before=rom;
        nes::Host host;
        host.service=[]{++services;};
        host.input=[](uint8_t &a,uint8_t &b,bool &quit) {
            a=frames>=start_frame && (frames-start_frame)%240<12?8:0;
            b=0; quit=frames>=limit;
        };
        host.line=[](unsigned y,const uint16_t *p,uint8_t mask,bool pal) {
            for(unsigned x=0;x<256;++x) {
                const auto token=presentation::pixel_token(p[x],mask,pal);
                hash=(hash^token)*16777619u;
                screen[y*256+x]=presentation::token_rgb565(token);
            }
        };
        host.frame=[]{++frames; last_hash=hash; hash=2166136261u;};
        host.message=[](const char *s){std::printf("core: %s\n",s);};
        const char *error=nullptr;
        const auto started=std::chrono::steady_clock::now();
        const bool ok=nes::infones_adapter::run(host,rom.data(),rom.size(),&error);
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        std::printf("ok=%d error=%s frames=%u hash=%08x services=%u allocations=%u ROM_unchanged=%d host_seconds=%.6f\n",
            ok,error?error:"none",frames,last_hash,services,allocations,rom==before,seconds);
        if(!ok || allocations || rom!=before || frames!=limit) return 1;
        if(!output.empty()) {
            std::ofstream ppm(output,std::ios::binary); ppm<<"P6\n256 240\n255\n";
            for(auto pixel:screen) {
                const char rgb[]={char(((pixel>>11)&31)*255/31),char(((pixel>>5)&63)*255/63),char((pixel&31)*255/31)};
                ppm.write(rgb,3);
            }
            ppm.close(); if(!ppm) throw std::runtime_error("Cannot write screenshot");
        }
        return 0;
    } catch(const std::exception &e) {std::cerr<<"ROM probe: "<<e.what()<<'\n'; return 1;}
}
