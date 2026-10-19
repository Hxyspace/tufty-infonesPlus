#include "storage.hpp"
#include "fat_disk.hpp"
#include "core_api/cartridge.hpp"
#include "menu.hpp"
#include "lfs.h"
#include <sys/mman.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>
using namespace tufty::storage::layout;
static uint8_t *flash;
void tufty_service() {}
namespace display {
static bool stopped=false;
bool paused() {return stopped;}
void pause(bool enabled) {stopped=enabled;}
}
void flash_range_erase(uint32_t at,size_t bytes) {
    assert(at%4096==0 && bytes%4096==0);
    assert((at>=kState.offset && kState.contains(at-kState.offset,bytes)) ||
           (at>=kSystem.offset && kSystem.contains(at-kSystem.offset,bytes)));
    std::memset(flash+at,255,bytes);
}
void flash_range_program(uint32_t at,const uint8_t *data,size_t bytes) {
    assert(at%256==0 && bytes%256==0 && at>=kSystem.offset && at+bytes<=16*1024*1024);
    for(size_t i=0;i<bytes;++i) {assert((flash[at+i]&data[i])==data[i]); flash[at+i]=data[i];}
}
static int read_lfs(const lfs_config *,lfs_block_t b,lfs_off_t at,void *out,lfs_size_t n) {
    std::memcpy(out,flash+kState.offset+b*4096+at,n); return 0;
}
static int prog_lfs(const lfs_config *,lfs_block_t b,lfs_off_t at,const void *p,lfs_size_t n) {
    flash_range_program(kState.offset+b*4096+at,static_cast<const uint8_t *>(p),n); return 0;
}
static int erase_lfs(const lfs_config *,lfs_block_t b) {flash_range_erase(kState.offset+b*4096,4096); return 0;}
static int sync_lfs(const lfs_config *) {return 0;}
static void fixture_state(bool corrupt,const char *idle=nullptr,const char *ble=nullptr) {
    lfs_config cfg{}; uint8_t r[256],p[256],look[32],fbuf[256];
    cfg.read=read_lfs; cfg.prog=prog_lfs; cfg.erase=erase_lfs; cfg.sync=sync_lfs;
    cfg.read_size=cfg.prog_size=cfg.cache_size=256; cfg.block_size=4096;
    cfg.block_count=256; cfg.block_cycles=500; cfg.lookahead_size=32;
    cfg.read_buffer=r; cfg.prog_buffer=p; cfg.lookahead_buffer=look;
    lfs_t fs{}; assert(lfs_format(&fs,&cfg)==0); assert(lfs_mount(&fs,&cfg)==0);
    assert(lfs_mkdir(&fs,"state")==0);
    lfs_file_t f{}; lfs_file_config fc{}; fc.buffer=fbuf;
    assert(lfs_file_opencfg(&fs,&f,"/state/settings.json",LFS_O_WRONLY|LFS_O_CREAT,&fc)==0);
    const char value[]="{\"brightness\":42}";
    assert(lfs_file_write(&fs,&f,value,sizeof(value)-1)==sizeof(value)-1);
    assert(lfs_file_close(&fs,&f)==0);
    f={};
    assert(lfs_file_opencfg(&fs,&f,"/state/nes.json",LFS_O_WRONLY|LFS_O_CREAT,&fc)==0);
    std::string nes=corrupt?"{broken":"{\"controller_id\":23,\"brightness\":67,\"swap_ab\":true,\"last_rom\":\"Sample Game.nes\"}";
    if(idle) {nes.pop_back(); nes+=",\"menu_sleep_seconds\":"; nes+=idle; nes+='}';}
    if(ble) {nes.pop_back(); nes+=','; nes+=ble; nes+='}';}
    assert(lfs_file_write(&fs,&f,nes.data(),nes.size())==static_cast<lfs_ssize_t>(nes.size()));
    assert(lfs_file_close(&fs,&f)==0); assert(lfs_unmount(&fs)==0);
}
int main(int argc,char **argv) {
    assert(argc==3 || argc==4);
    flash=static_cast<uint8_t *>(mmap(reinterpret_cast<void *>(kXipBase),16*1024*1024,
        PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0));
    assert(flash!=MAP_FAILED && reinterpret_cast<uintptr_t>(flash)==kXipBase);
    std::memset(flash,255,16*1024*1024);
    std::ifstream input(argv[1],std::ios::binary); assert(input);
    input.read(reinterpret_cast<char *>(flash+kSystem.offset),kSystem.bytes); assert(input.gcount()==kSystem.bytes);
    std::vector<uint8_t> original(flash,flash+16*1024*1024);
    std::string scenario=argc==4?argv[3]:"";
    bool gamepad_no_state=scenario=="gamepad_no_state";
    bool blank=argc==4 && (std::string(argv[3])=="blank" || gamepad_no_state);
    bool corrupt=argc==4 && std::string(argv[3])=="corrupt";
    bool idle_off=argc==4 && std::string(argv[3])=="idle_off";
    bool bad_idle=argc==4 && std::string(argv[3])=="bad_idle";
    bool custom_ble=argc==4 && (std::string(argv[3])=="ble" || scenario.rfind("gamepad_",0)==0);
    bool bad_peer=argc==4 && std::string(argv[3])=="bad_peer";
    bool bad_ble=argc==4 && std::string(argv[3])=="bad_ble";
    bool policy=scenario=="policy";
    bool bad_policy=scenario=="bad_policy" || scenario=="duplicate_peer";
    const char *ble=custom_ble?"\"ble_enabled\":false,\"ble_peer\":\"AA:BB:CC:DD:EE:FF\",\"ble_random\":true":
        bad_peer?"\"ble_peer\":\"garbage\"":bad_ble?"\"ble_enabled\":1":nullptr;
    if(policy) ble="\"serial_enabled\":false,\"ble_mode\":\"whitelist\",\"ble_preferred\":1,\"ble_whitelist\":[{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"random\":false},{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"random\":true}]";
    if(bad_policy) ble="\"serial_enabled\":false,\"ble_mode\":\"whitelist\",\"ble_preferred\":0,\"ble_whitelist\":[]";
    if(scenario=="duplicate_peer") ble="\"ble_whitelist\":[{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"random\":false},{\"mac\":\"aa:bb:cc:dd:ee:ff\",\"random\":false}]";
    if(!blank) fixture_state(corrupt,idle_off?"0":bad_idle?"86401":nullptr,ble);
    if(blank || corrupt || bad_idle || bad_peer || bad_ble || bad_policy) {
        std::vector<uint8_t> before(flash,flash+16*1024*1024);
        storage::init(); assert(storage::mounted() && !storage::state_ready());
        assert(!storage::config.ble.count && storage::config.serial_enabled);
        assert(!storage::save());
        assert(std::memcmp(flash,before.data(),before.size())==0);
        return 0;
    }
    storage::init(); assert(storage::mounted() && storage::state_ready());
    assert(storage::config.controller==23 && storage::config.brightness==67 && storage::config.swap_ab);
    assert(std::string(storage::config.last_rom)=="Sample Game.nes");
    assert(storage::config.menu_sleep_seconds==(idle_off?0u:300u)); // Old/default and disabled settings.
    assert(storage::config.ble_enabled==!custom_ble);
    assert(storage::config.serial_enabled==!policy);
    assert(storage::config.ble.count==(policy?2:custom_ble?1:0));
    if(custom_ble || policy) {
        assert(storage::config.ble.mode==bluetooth::Mode::WhitelistOnly);
        assert(storage::config.ble.preferred==(policy?1:0));
        assert(storage::config.ble.peers[custom_ble?0:1].random);
        char mac[18]; storage::config.ble.peers[0].text(mac);
        assert(std::string(mac)=="AA:BB:CC:DD:EE:FF");
    }
    assert(storage::config.ble_scan_uuid==0x1812);
    assert(flashfat::disk.sector_size==std::stoi(argv[2]));
    storage::scan(); assert(storage::count==2);
    bool unicode=false;
    for(size_t i=0;i<storage::count;++i) if(std::string(storage::entries[i].name)=="测试.nes") unicode=true;
    assert(unicode);
    const uint8_t *data; size_t bytes;
    assert(storage::load(0,data,bytes) && bytes==16400);
    nes::Cartridge image; assert(nes::inspect(data,bytes,image));
    storage::config.controller=65535; storage::config.brightness=35; storage::config.swap_ab=true;
    storage::config.menu_sleep_seconds=0;
    storage::config.ble_enabled=false; storage::config.serial_enabled=false;
    storage::config.ble_scan_uuid=0x1812;
    storage::config.ble={}; bluetooth::Peer key;
    assert(bluetooth::address("AA:BB:CC:DD:EE:FF",key.address)); key.random=true;
    assert(storage::config.ble.add(key)); storage::config.ble.preferred=0;
    storage::config.ble.mode=bluetooth::Mode::WhitelistOnly;
    std::strcpy(storage::config.last_rom,"测试.nes");
    assert(storage::save());
    char text[1024]; size_t actual;
    assert(storage::read_file("/state/nes.json",text,sizeof(text)-1,actual)); text[actual]=0;
    assert(std::strstr(text,"65535") && std::strstr(text,"测试.nes"));
    assert(std::strstr(text,"\"menu_sleep_seconds\":0"));
    assert(std::strstr(text,"\"ble_enabled\":false") && std::strstr(text,"\"serial_enabled\":false") &&
           std::strstr(text,"\"ble_whitelist\":[{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"random\":true}]"));
    assert(std::strstr(text,"\"ble_mode\":\"whitelist\"") && std::strstr(text,"\"ble_preferred\":0"));
    assert(!std::strstr(text,"ble_peer\"") && !std::strstr(text,"ble_random\""));
    assert(std::strstr(text,"\"ble_scan_uuid\":\"0x1812\""));
    assert(storage::read_file("/state/settings.json",text,sizeof(text)-1,actual)); text[actual]=0;
    assert(std::string(text)=="{\"brightness\":42}");
    assert(!storage::read_file("/system/../state/settings.json",text,sizeof(text),actual));
    assert(std::memcmp(flash,original.data(),kState.offset)==0); // Firmware, ROMFS, FAT unchanged by State writes.
    uint32_t at;
    assert(!flashfat::disk.range(UINT32_MAX,0,1,at));
    assert(!flashfat::disk.range(0,4096,1,at));
    assert(!flashfat::disk.range(kSystem.bytes/flashfat::disk.sector_size-1,0,8192,at));
    std::vector<uint16_t> frame(320*240+32,0x1234);
    for(int i=0;i<10;++i) menu::draw(frame.data(),0,i/10.0f,300);
    for(size_t i=320*240;i<frame.size();++i) assert(frame[i]==0x1234);
    std::ofstream ppm(std::string(argv[1])+".ppm",std::ios::binary); ppm<<"P6\n320 240\n255\n";
    menu::draw(frame.data(),0,0,300);
    for(size_t i=0;i<320*240;++i) {uint16_t v=frame[i]; char c[]={char((v>>11)*255/31),char(((v>>5)&63)*255/63),char((v&31)*255/31)}; ppm.write(c,3);}
    if(const char *preview=std::getenv("TUFTY_PREVIEW")) {
        std::ofstream out(preview,std::ios::binary); out<<"P6\n320 240\n255\n";
        for(size_t i=0;i<320*240;++i) {uint16_t v=frame[i]; char c[]={char((v>>11)*255/31),char(((v>>5)&63)*255/63),char((v&31)*255/31)}; out.write(c,3);}
    }
    uint8_t staging[4096]{};
    assert(!storage::replace_system_sector(0,staging));
    assert(storage::unmount_system()); display::pause(true);
    assert(!storage::replace_system_sector(kSystem.bytes,staging));
    assert(!storage::replace_system_sector(1,staging));
    assert(storage::replace_system_sector(kSystem.bytes-4096,staging));
    assert(std::memcmp(flash+kSystem.offset,original.data()+kSystem.offset,kSystem.bytes-4096)==0);
    std::memset(flash+kSystem.offset+11,0,2);
    assert(!flashfat::disk.probe()); // Invalid BPB does not trigger a format.
    munmap(flash,16*1024*1024);
}
