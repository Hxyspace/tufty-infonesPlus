#include "storage.hpp"
#include "fat_disk.hpp"
#include "core_api/cartridge.hpp"
#include "menu.hpp"
#include "menu_assets.hpp"
#include "input.hpp"
#include "platform/tufty2350/battery.hpp"
#include "cJSON.h"
#include "lfs.h"
#include <sys/mman.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>
#include <chrono>
namespace battery {extern Reading test_value; extern unsigned test_refreshes;}
using namespace tufty::storage::layout;
static uint8_t *flash;
static unsigned service_calls=0;
static bool check_read_ownership=false;
static unsigned blocked_unmounts=0;
void tufty_service() {
    ++service_calls;
    if(check_read_ownership) {assert(!storage::unmount_system()); ++blocked_unmounts;}
}
static void select_ready(int index) {
    menu::select(index);
    menu::load_current_cover(); // Most storage tests examine the prepared card.
}
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
    assert(argc>=3 && argc<=5);
    flash=static_cast<uint8_t *>(mmap(reinterpret_cast<void *>(kXipBase),16*1024*1024,
        PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0));
    assert(flash!=MAP_FAILED && reinterpret_cast<uintptr_t>(flash)==kXipBase);
    std::memset(flash,255,16*1024*1024);
    std::ifstream input(argv[1],std::ios::binary); assert(input);
    input.read(reinterpret_cast<char *>(flash+kSystem.offset),kSystem.bytes); assert(input.gcount()==kSystem.bytes);
    std::vector<uint8_t> original(flash,flash+16*1024*1024);
    std::string scenario=argc>=4?argv[3]:"";
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
    if(scenario=="catalog") {
        assert(argc==5);
        std::ifstream expected_file(argv[4]);
        std::string expected_text((std::istreambuf_iterator<char>(expected_file)),{});
        auto *expected=cJSON_Parse(expected_text.c_str()); assert(cJSON_IsArray(expected));
        using Clock=std::chrono::steady_clock;
        auto began=Clock::now(); storage::scan();
        auto us=[](auto start) {return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count();};
        std::printf("HOST catalog read+validate: %lld us\n",(long long)us(began));
        assert(storage::indexed() && storage::count==size_t(cJSON_GetArraySize(expected)));
        menu::load_art();
        for(size_t i=0;i<storage::count;++i) {
            auto *record=cJSON_GetArrayItem(expected,int(i));
            assert(std::string(storage::entries[i].name)==cJSON_GetObjectItemCaseSensitive(record,"rom")->valuestring);
            assert(std::string(storage::title(i))==cJSON_GetObjectItemCaseSensitive(record,"title")->valuestring);
            assert(std::string(storage::description(i))==cJSON_GetObjectItemCaseSensitive(record,"description")->valuestring);
            assert(storage::entries[i].flags==unsigned(cJSON_GetObjectItemCaseSensitive(record,"flags")->valueint));
            const uint8_t *rom_data; size_t rom_bytes;
            assert(storage::load(i,rom_data,rom_bytes) && rom_bytes>=16 && !std::memcmp(rom_data,"NES\x1a",4));
            const auto hits_before=menu::timing.cover_hits;
            began=Clock::now(); select_ready(int(i)); auto prepare_us=us(began);
            if(i) assert(menu::timing.cover_hits==hits_before+1);
            std::vector<uint16_t> frame(320*240+32,0x1234);
            began=Clock::now(); menu::draw(frame.data(),int(i),float(i),300); auto draw_us=us(began);
            if(const char *directory=std::getenv("TUFTY_PREVIEW_DIR")) {
                std::ofstream out(std::string(directory)+"/"+std::to_string(i)+".ppm",std::ios::binary); out<<"P6\n320 240\n255\n";
                for(size_t p=0;p<320*240;++p) {uint16_t v=frame[p]; char c[]={char((v>>11)*255/31),char(((v>>5)&63)*255/63),char((v&31)*255/31)}; out.write(c,3);}
            }
            began=Clock::now(); menu::next_page(); auto page_us=us(began);
            for(int step=0;step<10;++step) menu::draw(frame.data(),int(i),float(i)-step/10.0f,300);
            for(size_t p=320*240;p<frame.size();++p) assert(frame[p]==0x1234);
            const uint32_t idle=uint32_t(i)*1000;
            assert(!menu::prefetch(idle,false));
            menu::prefetch(idle+50,false);
            menu::prefetch(idle+51,false);
            assert(!menu::prefetch(idle+52,false)); // Three-slot eviction converges to current +/- 1.
            std::printf("HOST game %zu prepare=%lld page=%lld draw=%lld us: %s\n",i,(long long)prepare_us,(long long)page_us,(long long)draw_us,storage::entries[i].name);
        }
        cJSON_Delete(expected); munmap(flash,16*1024*1024); return 0;
    }
    if(scenario.rfind("index_",0)==0) {
        storage::scan();
        const bool valid=scenario=="index_valid" || scenario=="index_empty" || scenario=="index_max" || scenario=="index_long";
        assert(storage::indexed()==valid);
        if(valid) {
            assert(storage::count==(scenario=="index_empty"?0u:scenario=="index_max"?128u:2u));
            if(storage::count) {
                assert(std::string(storage::entries[0].name)=="Absent.NES");
                assert(std::string(storage::title(0))=="Z Display Title");
                assert(storage::has_cover(0));
                assert(storage::entries[0].bytes==0); // No pre-opening ROMs while loading the index.
                const uint8_t *rom_data=nullptr; size_t rom_bytes=0;
                assert(!storage::load(0,rom_data,rom_bytes)); // Missing ROM is checked on launch.
                assert(std::string(storage::error)=="ROM missing");
                assert(storage::load(1,rom_data,rom_bytes) && rom_bytes==16400);
                assert(std::string(storage::title(1))=="A Display Title");
                assert(!*storage::description(1) && !storage::has_cover(1));
                menu::load_art();
                menu::select(0);
                std::vector<uint16_t> placeholder(320*240);
                menu::draw(placeholder.data(),0,0,300);
                assert(placeholder[80*320+24]!=0x001f); // No cover read before first presentation.
                assert(!menu::prefetch(0,false));
                assert(!menu::prefetch(100,false)); // Neighbor must not overtake pending current cover.
                assert(menu::load_current_cover());
                assert(!menu::load_current_cover());
                const auto samples=battery::test_refreshes;
                menu::enter(10); menu::service(5009); assert(battery::test_refreshes==samples+1);
                menu::service(5010); assert(battery::test_refreshes==samples+2);
                std::vector<uint16_t> first(320*240+32,0x1234), frame(first);
                const unsigned before=service_calls;
                menu::draw(first.data(),0,0,300);
                static_assert(sizeof(art::background)==320*240*2);
                assert(first[0]==art::background[0]); // Header overlays the full-screen background.
                assert(service_calls==before); // draw must not perform storage I/O, ADC or layout service.
                assert(battery::test_refreshes==samples+2);
                auto rgb=[](unsigned r,unsigned g,unsigned b) {return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);};
                assert(first[20*320+305]==rgb(100,100,100));
                assert(first[18*320+274]==art::background[18*320+274]); // Empty battery outline reveals background.
                battery::test_value.valid=true; battery::test_value.percent=100;
                menu::draw(frame.data(),0,0,300); assert(frame[18*320+274]==rgb(0,166,106));
                battery::test_value.valid=false;
                gamepad::Receiver serial,ble; input::hub.attach(input::Source::Serial,serial); input::hub.attach(input::Source::Ble,ble);
                serial.update({},0); menu::draw(frame.data(),0,0,300); assert(frame[20*320+305]==rgb(65,145,255));
                ble.update({},1); menu::draw(frame.data(),0,0,300); assert(frame[20*320+305]==rgb(65,145,255));
                serial.release(); menu::draw(frame.data(),0,0,300); assert(frame[20*320+305]==rgb(70,220,100));
                ble.release(); menu::draw(frame.data(),0,0,300); assert(frame[20*320+305]==rgb(100,100,100));
                assert(first[80*320+24]==0x001f); // LE .565 cover is used at the new dimensions.
                menu::draw(frame.data(),0,0,300,"ROM missing");
                assert(frame[80*320+24]==0x001f); // Failure keeps the selected card and cover.
                assert(frame!=first);
                menu::next_page(); menu::draw(frame.data(),0,0,300);
                assert(frame!=first);
                menu::previous_page(); menu::draw(frame.data(),0,0,300); assert(frame==first);
                for(unsigned i=0;i<1024;++i) menu::next_page();
                menu::draw(frame.data(),0,0,300); auto last=frame;
                menu::next_page(); menu::draw(frame.data(),0,0,300); assert(frame==last);
                assert(frame!=first);
                for(unsigned i=0;i<1024;++i) menu::previous_page();
                menu::draw(frame.data(),0,0,300); assert(frame==first);
                for(size_t i=320*240;i<frame.size();++i) assert(frame[i]==0x1234);
                assert(!menu::prefetch(0,false)); // Debounce selection before speculative I/O.
                assert(!menu::prefetch(49,false));
                assert(!menu::prefetch(50,true)); // Held navigation postpones prefetch.
                assert(!menu::prefetch(99,false));
                assert(menu::prefetch(100,false));
                uint16_t cover[58*80]; size_t cover_bytes=0;
                char cover_path[280]; std::snprintf(cover_path,sizeof(cover_path),"/system/roms/%s",storage::entries[0].name);
                std::strcpy(std::strrchr(cover_path,'.'),".565");
                check_read_ownership=true;
                assert(storage::read_file(cover_path,cover,sizeof(cover),cover_bytes));
                check_read_ownership=false; assert(blocked_unmounts>0 && storage::mounted());
                assert(!menu::prefetch(101,false)); // Two-game library: no duplicate/failed-load retries.
                const auto hits=menu::timing.cover_hits;
                const auto misses=menu::timing.cover_misses;
                select_ready(1);
                assert(menu::timing.cover_hits==hits+1 && menu::timing.cover_misses==misses);
                menu::draw(frame.data(),1,0.1f,300);
                assert(frame[80*320+5]==0x001f); // Outgoing card keeps its own cover while sliding.
                menu::next_page(); menu::draw(frame.data(),1,1,300); last=frame;
                menu::previous_page(); menu::draw(frame.data(),1,1,300); assert(frame==last);
                select_ready(0); menu::draw(frame.data(),0,0,300); assert(frame==first);
                menu::draw(frame.data(),0,0,1300);
                for(unsigned y=153;y<211;++y) for(unsigned x=0;x<100;++x)
                    assert(frame[y*320+x]==first[y*320+x]); // Labels do not bob with the cover.
                menu::draw(frame.data(),0,0,300);
                if(const char *preview=std::getenv("TUFTY_PREVIEW")) {
                    std::ofstream out(preview,std::ios::binary); out<<"P6\n320 240\n255\n";
                    for(size_t i=0;i<320*240;++i) {uint16_t v=frame[i]; char c[]={char((v>>11)*255/31),char(((v>>5)&63)*255/63),char((v&31)*255/31)}; out.write(c,3);}
                }
            }
        } else {
            assert(storage::count==2);
            assert(!storage::title(0) && !storage::description(0));
            assert(std::string(storage::entries[0].name)=="Sample Game.nes");
        }
        const auto generation=storage::generation();
        storage::scan(true); assert(!storage::indexed() && storage::count==2);
        assert(storage::generation()!=generation);
        select_ready(0); // Invalidate cover and text caches after a directory generation change.
        munmap(flash,16*1024*1024); return 0;
    }
    assert(storage::config.controller==23 && storage::config.brightness==67 && storage::config.swap_ab);
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
    assert(storage::save());
    char text[1024]; size_t actual;
    assert(storage::read_file("/state/nes.json",text,sizeof(text)-1,actual)); text[actual]=0;
    assert(std::strstr(text,"65535") && !std::strstr(text,"last_rom"));
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
