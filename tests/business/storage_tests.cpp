#include "storage.hpp"
#include "library.hpp"
#include "game_lists.hpp"
#include "display.hpp"
#include "fat_disk.hpp"
#include "core_api/cartridge.hpp"
#include "menu.hpp"
#include "input.hpp"
#include "platform/tufty2350/battery.hpp"
#include "cJSON.h"
#include "lfs.h"
#include "hardware/sync.h"
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
static bool fail_flash=false;
struct PowerCut {};
static int cut_after=-1;
static unsigned programs=0,system_reads=0,progress_frames=0;
static flashfat::Read source_read;
static bool counted_read(uint32_t at,void *out,uint32_t n) {++system_reads; return source_read(at,out,n);}
static void progress() {++progress_frames; assert(!display::paused());}
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
namespace disk_display {
bool running=false;
bool active() {return running;}
}
void flash_range_erase(uint32_t at,size_t bytes) {
    if(fail_flash) return;
    if(disk_display::active())assert(flash_test::irq_disabled);
    assert(at%4096==0 && bytes%4096==0);
    assert((at>=kState.offset && kState.contains(at-kState.offset,bytes)) ||
           (at>=kSystem.offset && kSystem.contains(at-kSystem.offset,bytes)));
    const bool cut=cut_after>=0 && cut_after--==0;
    std::memset(flash+at,255,cut?bytes/2:bytes);
    if(cut) throw PowerCut{};
}
void flash_range_program(uint32_t at,const uint8_t *data,size_t bytes) {
    ++programs;
    if(fail_flash) return;
    if(disk_display::active())assert(flash_test::irq_disabled);
    assert(at%256==0 && bytes%256==0 && at>=kSystem.offset && at+bytes<=16*1024*1024);
    const bool cut=cut_after>=0 && cut_after--==0;
    for(size_t i=0;i<(cut?bytes/2:bytes);++i) {assert((flash[at+i]&data[i])==data[i]); flash[at+i]=data[i];}
    if(cut) throw PowerCut{};
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
    if(argc==4 && std::string(argv[3])=="packed_state_churn") {
        input.read(reinterpret_cast<char *>(flash),16*1024*1024);
        assert(input.gcount()==16*1024*1024);
        storage::init(); assert(storage::mounted() && storage::state_ready());
        assert(storage::scan() && storage::count>1);
        assert(library::move_to_front(storage::count-1) && library::save());
        assert(library::custom_order());
        // Force metadata compaction so both halves of /state's pair contain
        // valid old revisions, as on a device with repeated settings/order edits.
        for(uint32_t revision=0;revision<80;++revision)
            assert(storage::write_state("/state/replace-churn",&revision,sizeof revision));
        std::ofstream output(argv[1],std::ios::binary|std::ios::trunc);
        output.write(reinterpret_cast<const char *>(flash),16*1024*1024);
        output.close(); assert(output);
        munmap(flash,16*1024*1024); return 0;
    }
    if(argc==4 && std::string(argv[3])=="library_recover") {
        input.read(reinterpret_cast<char *>(flash),16*1024*1024); assert(input.gcount()==16*1024*1024);
        storage::init(); assert(storage::state_ready());
        assert(storage::scan() && storage::indexed() && storage::count==6);
        assert(std::string(storage::entries[0].name)=="A.nes" || std::string(storage::entries[0].name)=="C.nes");
        size_t n=0; assert(storage::file_size("/state/library.dirty",n)==storage::FileStatus::Missing);
        const auto writes=programs; assert(storage::scan() && programs==writes);
        munmap(flash,16*1024*1024); return 0;
    }
    if(argc==4 && std::string(argv[3])=="packed") {
        input.read(reinterpret_cast<char *>(flash),16*1024*1024);
        assert(input.gcount()==16*1024*1024);
        storage::init(); assert(storage::mounted() && storage::state_ready());
        const bool defaults=std::string(argv[2])=="defaults";
        assert(storage::config.brightness==(defaults?80:63));
        std::vector<uint8_t> background(320*240*2); size_t bytes=0;
        assert(storage::read_file("/rom/background/background.565",background.data(),background.size(),bytes));
        assert(bytes==background.size());
        if(!defaults) {
            char text[32]{};
            assert(storage::read_file("/system/assets/test.txt",text,sizeof(text),bytes));
            assert(std::string(text,bytes)=="system image test");
            assert(storage::read_file("/state/nested/test.txt",text,sizeof(text),bytes));
            assert(std::string(text,bytes)=="state image test");
            for(const auto *prefix:{"/system/assets/","/state/"}) {
                for(const auto *name:{"zeros.bin","erased.bin"}) {
                    std::vector<uint8_t> content(8192);
                    const auto path=std::string(prefix)+name;
                    assert(storage::read_file(path.c_str(),content.data(),content.size(),bytes));
                    assert(bytes==content.size());
                    for(auto value:content) assert(value==(name[0]=='z'?0:255));
                }
            }
        } else {
            char text[32]{};
            assert(!storage::read_file("/system/assets/test.txt",text,sizeof(text),bytes));
            assert(!storage::read_file("/state/nested/test.txt",text,sizeof(text),bytes));
            assert(storage::file_size("/state/game-order.json",bytes)==storage::FileStatus::Missing);
            assert(storage::file_size("/state/games.idx",bytes)==storage::FileStatus::Missing);
            assert(storage::file_size("/state/replace-churn",bytes)==storage::FileStatus::Missing);
        }
        menu::load_art(); std::vector<uint16_t> frame(320*240);
        menu::draw(frame.data(),0,0,300);
        assert(frame[0]==uint16_t(background[0]|(background[1]<<8)));
        assert(storage::save()); // Newly formatted state is writable by actual firmware code.
        munmap(flash,16*1024*1024); return 0;
    }
    input.read(reinterpret_cast<char *>(flash+kSystem.offset),kSystem.bytes); assert(input.gcount()==kSystem.bytes);
    std::vector<uint8_t> original(flash,flash+16*1024*1024);
    std::string scenario=argc>=4?argv[3]:"";
    bool gamepad_no_state=scenario=="gamepad_no_state";
    bool blank=argc==4 && (scenario=="blank" || gamepad_no_state || scenario=="reset_blank");
    bool corrupt=argc==4 && (scenario=="corrupt" || scenario=="reset_corrupt");
    bool idle_off=argc==4 && std::string(argv[3])=="idle_off";
    bool bad_idle=argc==4 && std::string(argv[3])=="bad_idle";
    const bool bad_palette=scenario=="bad_palette" || scenario=="bad_palette_type";
    const bool bad_background=scenario=="bad_background" || scenario=="bad_background_type";
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
    if(bad_palette) ble=scenario=="bad_palette"?"\"nes_palette\":\"unknown\"":"\"nes_palette\":2";
    if(bad_background) ble=scenario=="bad_background"?"\"menu_background\":\"/system/roms/game.565\"":"\"menu_background\":2";
    if(!blank) fixture_state(corrupt,idle_off?"0":bad_idle?"86401":nullptr,ble);
    if(scenario.rfind("reset",0)==0) {
        storage::init(); assert(storage::mounted());
        if(!blank && !corrupt) {
            assert(storage::scan());
            const char order[]="[\"Sample Game.nes\"]";
            assert(storage::write_state("/state/game-order.json",order,sizeof order-1));
            assert(storage::write_state("/state/extra",order,sizeof order-1));
        }
        const std::vector<uint8_t> before(flash,flash+kState.offset);
        if(scenario=="reset_failure") {
            fail_flash=true; assert(!storage::reset_state()); fail_flash=false;
            assert(!display::paused() && !storage::state_ready());
        }
        assert(storage::reset_state());
        assert(!display::paused() && storage::state_ready());
        size_t bytes=0;
        for(const auto *name:{"nes.json","settings.json","game-order.json","games.idx","library.stamp","extra"}) {
            const auto path=std::string("/state/")+name;
            assert(storage::file_size(path.c_str(),bytes)==storage::FileStatus::Missing);
        }
        assert(!std::memcmp(flash,before.data(),before.size())); // Firmware/ROM/System preserved.
        storage::config={}; assert(storage::save()); // Fresh State accepts default settings.
        storage::config.brightness=1; storage::init(); assert(storage::config.brightness==80);
        assert(storage::scan() && storage::count>0); // Missing index is rebuilt normally.
        assert(storage::reset_state()); // Repeated resets also clear the regenerated files.
        assert(storage::file_size("/state/nes.json",bytes)==storage::FileStatus::Missing);
        assert(storage::file_size("/state/games.idx",bytes)==storage::FileStatus::Missing);
        munmap(flash,16*1024*1024); return 0;
    }
    if(blank || corrupt || bad_idle || bad_peer || bad_ble || bad_policy || bad_palette || bad_background) {
        std::vector<uint8_t> before(flash,flash+16*1024*1024);
        storage::init(); assert(storage::mounted() && !storage::state_ready());
        assert(!storage::config.ble.count && storage::config.serial_enabled);
        assert(!storage::save());
        if(blank) {
            assert(!storage::scan() && storage::indexed() && storage::count==1);
            assert(std::string(storage::error)=="Game list ready; not saved");
        }
        assert(std::memcmp(flash,before.data(),before.size())==0);
        return 0;
    }
    storage::init(); assert(storage::mounted() && storage::state_ready());
    if(scenario=="library_limits") {
        assert(storage::scan() && storage::count==128 && storage::indexed());
        assert(std::string(storage::entries[0].name)=="L1.nes" && std::string(storage::entries[127].name)=="L128.nes");
        for(size_t i=0;i<5;++i) assert(!*storage::description(i));
        assert(std::string(storage::description(5))=="literal \\u0000 [[[]]]");
        const char order[]="[\"L140.nes\"]";
        assert(storage::mark_library_changed());
        assert(storage::write_state("/state/game-order.json",order,sizeof order-1));
        assert(storage::scan() && storage::count==128);
        assert(std::string(storage::entries[0].name)=="L140.nes" && std::string(storage::entries[127].name)=="L127.nes");
        assert(library::reset_order() && std::string(storage::entries[127].name)=="L128.nes");
        munmap(flash,16*1024*1024); return 0;
    }
    if(scenario=="library" || scenario.rfind("library_cut_",0)==0) {
        source_read=flashfat::disk.read; flashfat::disk.read=counted_read;
        const auto names=[] {
            std::string result;
            for(size_t i=0;i<storage::count;++i) {if(i)result+=","; result+=storage::entries[i].name;}
            return result;
        };
        const auto state=[](const char *path) {
            size_t n=0; std::vector<uint8_t> data(256*1024);
            assert(storage::read_file(path,data.data(),data.size(),n)); data.resize(n); return data;
        };
        const auto changed=[] {
            size_t n=0; return storage::file_size("/state/library.dirty",n)==storage::FileStatus::File;
        };
        assert(storage::scan(false,progress));
        assert(storage::indexed() && names()=="A.nes,B.nes,C.nes,D.nes,Game2.nes,Game10.nes");
        assert(std::string(storage::description(0))=="First\n\nSecond");
        assert(progress_frames && !changed() && !library::custom_order());
        if(scenario=="library") {
            assert(game_lists::open());
            assert(game_lists::toggle(2) && game_lists::toggle(0) && game_lists::save());
            game_lists::close(); assert(game_lists::open());
            game_lists::view(game_lists::View::Favorites);
            assert(game_lists::count()==2 && game_lists::game(0)==0 && game_lists::game(1)==2);
            assert(library::move_to_front(3)); game_lists::refresh();
            assert(game_lists::game(0)==1 && game_lists::game(1)==3);
            library::discard(); game_lists::refresh();
            assert(game_lists::game(0)==0 && game_lists::game(1)==2);
            alignas(8) uint8_t workspace[16*1024];
            assert(game_lists::suspend(workspace,sizeof workspace,3));
            game_lists::started(); assert(!game_lists::pending() && !game_lists::session_bytes());
            assert(game_lists::resume()); game_lists::close(); assert(game_lists::open());
            game_lists::view(game_lists::View::Recent);
            assert(game_lists::count()==1 && game_lists::game(0)==3);
            game_lists::close();
        }
        menu::load_art();
        std::vector<uint16_t> loading(320*240+16,0x1234);
        const auto services=service_calls;
        menu::draw_loading(loading.data(),10000); const auto first_loading=loading;
        menu::draw_loading(loading.data(),10070); assert(loading!=first_loading && service_calls==services);
        for(size_t i=320*240;i<loading.size();++i) assert(loading[i]==0x1234);
        if(const char *directory=std::getenv("TUFTY_PREVIEW_DIR")) {
            std::ofstream out(std::string(directory)+"/scanning.ppm",std::ios::binary); out<<"P6\n320 240\n255\n";
            for(size_t i=0;i<320*240;++i) {
                const auto pixel=loading[i];
                const char rgb[]={char(((pixel>>11)&31)*255/31),char(((pixel>>5)&63)*255/63),char((pixel&31)*255/31)};
                out.write(rgb,3);
            }
            assert(out);
        }
        if(scenario.rfind("library_cut_",0)==0) {
            cut_after=std::stoi(scenario.substr(12));
            assert(library::move_to_front(2));
            try {library::save();} catch(const PowerCut &) {}
            std::ofstream out(std::string(argv[4])+"/cut.img",std::ios::binary);
            out.write(reinterpret_cast<const char *>(flash),16*1024*1024); out.close(); assert(out);
            munmap(flash,16*1024*1024); return 0;
        }
        auto index=state("/state/games.idx");
        const auto writes=programs; system_reads=progress_frames=0;
        assert(storage::scan(false,progress));
        assert(!system_reads && !progress_frames && writes==programs); // Ordinary boot has no source reads/writes.
        assert(storage::mark_library_changed() && changed());
        assert(storage::scan());
        assert(system_reads && !changed() && state("/state/games.idx")==index);
        system_reads=0;
        const auto before_edits=programs;
        assert(library::move_to_front(2) && library::pending());
        assert(library::reset_order() && !library::pending());
        assert(names()=="A.nes,B.nes,C.nes,D.nes,Game2.nes,Game10.nes" && programs==before_edits);
        assert(library::move_to_front(2));
        assert(names()=="C.nes,A.nes,B.nes,D.nes,Game2.nes,Game10.nes" && !system_reads);
        assert(library::move_to_front(3));
        assert(names()=="D.nes,C.nes,A.nes,B.nes,Game2.nes,Game10.nes");
        assert(library::move_to_front(1));
        assert(library::pending() && programs==before_edits && !system_reads && !changed());
        assert(state("/state/games.idx")==index);
        assert(library::save() && !library::pending());
        auto order=state("/state/game-order.json");
        assert(std::string(order.begin(),order.end())=="[\"C.nes\",\"D.nes\"]");
        const auto unchanged_writes=programs;
        assert(library::move_to_front(0) && programs==unchanged_writes);
        // Failed durable marker must not replace the previous index or order.
        index=state("/state/games.idx"); fail_flash=true;
        assert(library::move_to_front(2) && library::pending());
        assert(!library::save() && library::pending()); fail_flash=false;
        assert(state("/state/games.idx")==index && state("/state/game-order.json")==order);
        library::discard();
        assert(!library::pending() && names()=="C.nes,D.nes,A.nes,B.nes,Game2.nes,Game10.nes");
        assert(library::move_to_front(2));
        fail_flash=true; assert(!library::save()); fail_flash=false;
        assert(library::save() && !library::pending()); // Retry preserves the draft.
        // Model interruption after committing order but before committing index.
        assert(storage::mark_library_changed());
        const char interrupted[]="[\"B.nes\",\"C.nes\"]";
        assert(storage::write_state("/state/game-order.json",interrupted,sizeof interrupted-1));
        assert(storage::scan() && !changed());
        assert(names()=="B.nes,C.nes,A.nes,D.nes,Game2.nes,Game10.nes");
        // Emulate a completed USB session changing metadata and deleting a ROM.
        const auto replace_system=[&](const char *name) {
            assert(storage::mark_library_changed());
            assert(storage::unmount_system());
            std::ifstream file(std::string(argv[4])+"/"+name,std::ios::binary);
            file.read(reinterpret_cast<char *>(flash+kSystem.offset),kSystem.bytes);
            assert(file.gcount()==kSystem.bytes); assert(storage::init_disk());
        };
        replace_system("changed.img"); assert(storage::scan());
        assert(names()=="B.nes,A.nes,D.nes,Game2.nes,Game10.nes");
        assert(std::string(storage::description(1))=="First\n\nEdited"); // Same-size JSON edit changes fingerprint.
        replace_system("original.img"); assert(storage::scan());
        assert(names()=="B.nes,C.nes,A.nes,D.nes,Game2.nes,Game10.nes"); // Absent priority retained.
        assert(library::reset_order() && !library::custom_order());
        assert(names()=="A.nes,B.nes,C.nes,D.nes,Game2.nes,Game10.nes");
        assert(library::save());
        // Corrupt order is not overwritten; reset remains available from the in-memory catalog.
        assert(storage::mark_library_changed());
        assert(storage::write_state("/state/game-order.json","[broken",7));
        assert(!storage::scan() && storage::indexed() && storage::count==6 && changed());
        assert(library::reset_order()); assert(storage::scan() && !changed());
        replace_system("empty.img"); assert(storage::scan() && storage::indexed() && !storage::count);
        system_reads=0; const auto empty_writes=programs;
        assert(storage::scan() && !system_reads && programs==empty_writes);
        munmap(flash,16*1024*1024); return 0;
    }
    if(scenario=="catalog") {
        std::strcpy(storage::config.menu_background,"/system/assets/background/background.565");
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
            const char *expected_name=cJSON_GetObjectItemCaseSensitive(record,"rom")->valuestring;
            if(std::strcmp(storage::entries[i].name,expected_name))
                std::fprintf(stderr,"Catalog row %zu: expected '%s', got '%s'\n",i,expected_name,storage::entries[i].name);
            assert(std::string(storage::entries[i].name)==expected_name);
            assert(std::string(storage::title(i))==cJSON_GetObjectItemCaseSensitive(record,"title")->valuestring);
            assert(std::string(storage::description(i))==cJSON_GetObjectItemCaseSensitive(record,"description")->valuestring);
            assert(storage::entries[i].flags==unsigned(cJSON_GetObjectItemCaseSensitive(record,"flags")->valueint));
            const uint8_t *rom_data; size_t rom_bytes;
            assert(storage::load(i,rom_data,rom_bytes) && rom_bytes>=16 && !std::memcmp(rom_data,"NES\x1a",4));
            const auto hits_before=menu::timing.cover_hits;
            began=Clock::now(); select_ready(int(i)); auto prepare_us=us(began);
            if(i) assert(menu::timing.cover_hits==hits_before+1);
            std::vector<uint16_t> frame(320*240+32,0x1234);
            began=Clock::now(); menu::draw(frame.data(),int(i)+1,float(i)+1,300); auto draw_us=us(began);
            if(const char *directory=std::getenv("TUFTY_PREVIEW_DIR")) {
                std::ofstream out(std::string(directory)+"/"+std::to_string(i)+".ppm",std::ios::binary); out<<"P6\n320 240\n255\n";
                for(size_t p=0;p<320*240;++p) {uint16_t v=frame[p]; char c[]={char((v>>11)*255/31),char(((v>>5)&63)*255/63),char((v&31)*255/31)}; out.write(c,3);}
            }
            began=Clock::now(); menu::next_page(); auto page_us=us(began);
            for(int step=0;step<10;++step) menu::draw(frame.data(),int(i)+1,float(i)+1-step/10.0f,300);
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
        // Test the GIDX decoder independently of source reconstruction. A
        // System fixture is copied into State by the test, never by firmware.
        std::vector<uint8_t> wire(512*1024); size_t wire_bytes=0;
        assert(storage::read_file("/system/roms/games.idx",wire.data(),wire.size(),wire_bytes));
        assert(storage::write_state("/state/games.idx",wire.data(),wire_bytes));
        storage::load_catalog();
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
                menu::draw(placeholder.data(),1,1,300);
                assert(placeholder[80*320+24]!=0x001f); // No cover read before first presentation.
                assert(!menu::prefetch(0,false));
                assert(!menu::prefetch(100,false)); // Neighbor must not overtake pending current cover.
                assert(menu::load_current_cover());
                assert(!menu::load_current_cover());
                const auto samples=battery::test_refreshes;
                menu::enter(10); menu::service(30009); assert(battery::test_refreshes==samples+1);
                menu::service(30010); assert(battery::test_refreshes==samples+2);
                menu::enter(30011); assert(battery::test_refreshes==samples+2);
                std::vector<uint16_t> first(320*240+32,0x1234), frame(first);
                const unsigned before=service_calls;
                menu::draw(first.data(),1,1,300);
                assert(first[0]==0); // No resource background: black fallback.
                assert(service_calls==before); // draw must not perform storage I/O, ADC or layout service.
                assert(battery::test_refreshes==samples+2);
                auto rgb=[](unsigned r,unsigned g,unsigned b) {return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);};
                assert(first[20*320+305]==rgb(100,100,100));
                assert(first[18*320+274]==0); // Empty battery outline reveals background.
                battery::test_value.valid=true; battery::test_value.percent=100;
                menu::draw(frame.data(),1,1,300); assert(frame[18*320+274]==rgb(0,166,106));
                battery::test_value.valid=false;
                gamepad::Receiver serial,ble; input::hub.attach(input::Source::Serial,serial); input::hub.attach(input::Source::Ble,ble);
                serial.update({},0); menu::draw(frame.data(),1,1,300); assert(frame[20*320+305]==rgb(65,145,255));
                ble.update({},1); menu::draw(frame.data(),1,1,300); assert(frame[20*320+305]==rgb(65,145,255));
                serial.release(); menu::draw(frame.data(),1,1,300); assert(frame[20*320+305]==rgb(70,220,100));
                ble.release(); menu::draw(frame.data(),1,1,300); assert(frame[20*320+305]==rgb(100,100,100));
                assert(first[80*320+24]==0x001f); // LE .565 cover is used at the new dimensions.
                menu::draw(frame.data(),1,1,300,"ROM missing");
                assert(frame[80*320+24]==0x001f); // Failure keeps the selected card and cover.
                assert(frame!=first);
                menu::next_page(); menu::draw(frame.data(),1,1,300);
                assert(frame!=first);
                menu::previous_page(); menu::draw(frame.data(),1,1,300); assert(frame==first);
                for(unsigned i=0;i<1024;++i) menu::next_page();
                menu::draw(frame.data(),1,1,300); auto last=frame;
                menu::next_page(); menu::draw(frame.data(),1,1,300); assert(frame==last);
                assert(frame!=first);
                for(unsigned i=0;i<1024;++i) menu::previous_page();
                menu::draw(frame.data(),1,1,300); assert(frame==first);
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
                menu::draw(frame.data(),2,1.1f,300);
                assert(frame[80*320+5]==0x001f); // Outgoing card keeps its own cover while sliding.
                menu::next_page(); menu::draw(frame.data(),2,2,300); last=frame;
                menu::previous_page(); menu::draw(frame.data(),2,2,300); assert(frame==last);
                select_ready(0); menu::draw(frame.data(),1,1,300); assert(frame==first);
                menu::draw(frame.data(),1,1,1300);
                for(unsigned y=153;y<211;++y) for(unsigned x=0;x<100;++x)
                    assert(frame[y*320+x]==first[y*320+x]); // Labels do not bob with the cover.
                menu::draw(frame.data(),1,1,300);
                if(const char *preview=std::getenv("TUFTY_PREVIEW")) {
                    std::ofstream out(preview,std::ios::binary); out<<"P6\n320 240\n255\n";
                    for(size_t i=0;i<320*240;++i) {uint16_t v=frame[i]; char c[]={char((v>>11)*255/31),char(((v>>5)&63)*255/63),char((v&31)*255/31)}; out.write(c,3);}
                }
            }
        } else {
            storage::scan(true);
            assert(storage::indexed() && storage::count==1);
            assert(std::string(storage::entries[0].name)=="Sample Game.nes");
        }
        const auto generation=storage::generation();
        storage::scan(true); assert(storage::indexed() && storage::count==1);
        assert(storage::generation()!=generation);
        select_ready(0); // Invalidate cover and text caches after a directory generation change.
        munmap(flash,16*1024*1024); return 0;
    }
    assert(storage::config.controller==23 && storage::config.brightness==67 && storage::config.swap_ab);
    assert(storage::config.palette==presentation::Palette::Current); // Existing records need no migration.
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
    storage::scan(); assert(storage::indexed() && storage::count==1);
    assert(std::string(storage::entries[0].name)=="Sample Game.nes"); // Unsupported glyph names are reported and skipped.
    const uint8_t *data; size_t bytes;
    assert(storage::load(0,data,bytes) && bytes==16400);
    nes::Cartridge image; assert(nes::inspect(data,bytes,image));
    storage::config.controller=65535; storage::config.brightness=35; storage::config.swap_ab=true;
    storage::config.menu_sleep_seconds=0;
    storage::config.palette=presentation::Palette::Unsaturated;
    std::strcpy(storage::config.menu_background,"/system/assets/background/Missing image.565");
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
    assert(std::strstr(text,"\"nes_palette\":\"unsaturated-final\""));
    storage::config.palette=presentation::Palette::Current;
    storage::init(); assert(storage::config.palette==presentation::Palette::Unsaturated);
    assert(!std::strcmp(storage::config.menu_background,"/system/assets/background/Missing image.565"));
    assert(storage::state_ready()); // A missing image is not a corrupt configuration.
    for(unsigned i=0;i<presentation::palette_count;++i) {
        const auto selected=presentation::Palette(i);
        storage::config.palette=selected;
        assert(storage::save()); storage::config.palette=presentation::Palette::Count;
        storage::init(); assert(storage::config.palette==selected);
    }
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
    display::pause(false);
    assert(!storage::replace_system_sector(kSystem.bytes-4096,staging));
    disk_display::running=true; // Core1 has acknowledged the SRAM-only loop.
    staging[0]=0xa5;
    assert(storage::replace_system_sector(kSystem.bytes-4096,staging));
    assert(!display::paused() && !flash_test::irq_disabled);
    assert(flash[kSystem.offset+kSystem.bytes-4096]==0xa5);
    disk_display::running=false;
    assert(std::memcmp(flash+kSystem.offset,original.data()+kSystem.offset,kSystem.bytes-4096)==0);
    std::memset(flash+kSystem.offset+11,0,2);
    assert(!flashfat::disk.probe()); // Invalid BPB does not trigger a format.
    munmap(flash,16*1024*1024);
}
