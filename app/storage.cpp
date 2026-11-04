#include "storage.hpp"
#include "fat_disk.hpp"
#include "rom_store.hpp"
#include "display.hpp"
#include "disk_display.hpp"
#include "hardware/sync.h"
#include "bluetooth_config.hpp"
#include "ff.h"
#include "lfs.h"
#include "cJSON.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>
void tufty_service();
namespace storage {
using namespace tufty::storage::layout;
Config config;
char error[96]{};
static Entry __uninitialized_psram("rom_entries") table[kMaxRoms];
alignas(8) static uint8_t __uninitialized_psram("rom_data") rom_data[kMaxRomBytes];
static uint8_t __uninitialized_psram("games_index") index_data[256*1024];
Entry *entries=table;
size_t count=0;
static bool index_mode=false;
static uint32_t index_strings=0, index_file_bytes=0, storage_generation=0;
static FATFS fat;
static bool fat_mounted=false, state_mounted=false, writable_config=true;
// Core0 only. Nested service callbacks cannot hand FAT to USB while a file/directory is open.
static unsigned system_readers=0;
struct SystemReader {
    SystemReader() {++system_readers;}
    ~SystemReader() {--system_readers;}
};
static tufty::storage::RomStore rom;
static lfs_t lfs;
static lfs_config lc{};
static uint8_t read_cache[256], prog_cache[256], lookahead[32];
static bool read_system(uint32_t at,void *out,uint32_t bytes) {
    if(!kSystem.contains(at,bytes)) return false;
    std::memcpy(out,reinterpret_cast<const void *>(kXipBase+kSystem.offset+at),bytes); return true;
}
struct Operation {uint32_t at, bytes; const uint8_t *data; bool erase;};
static void __no_inline_not_in_flash_func(write_flash)(void *ctx) {
    auto &op=*static_cast<Operation *>(ctx);
    if(op.erase) flash_range_erase(op.at,op.bytes);
    if(op.data) flash_range_program(op.at,op.data,op.bytes);
}
static bool execute(Operation &op) {
    if(disk_display::active()) {
        // Only disk startup can acknowledge an IRQ-disabled SRAM-only core1.
        // Normal mode retains the SDK's multicore Flash lockout.
        const auto irq=save_and_disable_interrupts();
        write_flash(&op);
        restore_interrupts(irq);
        return true;
    }
    return flash_safe_execute(write_flash,&op,1000)==PICO_OK;
}
static bool state_range(lfs_block_t block,lfs_off_t at,lfs_size_t bytes) {
    return block<kState.bytes/4096 && at<=4096 && bytes<=4096-at;
}
static int state_read(const lfs_config *,lfs_block_t block,lfs_off_t at,void *out,lfs_size_t bytes) {
    if(!state_range(block,at,bytes)) return LFS_ERR_INVAL;
    std::memcpy(out,reinterpret_cast<const void *>(kXipBase+kState.offset+block*4096+at),bytes); return 0;
}
static int state_prog(const lfs_config *,lfs_block_t block,lfs_off_t at,const void *in,lfs_size_t bytes) {
    if(!state_range(block,at,bytes) || at%256 || bytes%256 || (!disk_display::active() && !display::paused())) return LFS_ERR_INVAL;
    alignas(4) uint8_t page[256];
    for(uint32_t n=0;n<bytes;n+=256) {
        std::memcpy(page,static_cast<const uint8_t *>(in)+n,256);
        Operation op{kState.offset+block*4096+at+n,256,page,false};
        if(!execute(op) || std::memcmp(reinterpret_cast<const void *>(kXipBase+op.at),page,256)) return LFS_ERR_IO;
    }
    return 0;
}
static int state_erase(const lfs_config *,lfs_block_t block) {
    if(!state_range(block,0,4096) || (!disk_display::active() && !display::paused())) return LFS_ERR_INVAL;
    Operation op{kState.offset+block*4096,4096,nullptr,true};
    if(!execute(op)) return LFS_ERR_IO;
    const auto *p=reinterpret_cast<const volatile uint8_t *>(kXipBase+op.at);
    for(size_t i=0;i<4096;++i) if(p[i]!=255) return LFS_ERR_IO;
    return 0;
}
static int state_sync(const lfs_config *) {return 0;}
static void load_config() {
    char text[2048]; size_t actual=0;
    // A missing NES record is normal; other applications' State files stay intact.
    lfs_info info{};
    int rc=lfs_stat(&lfs,"/state/nes.json",&info);
    if(rc==LFS_ERR_NOENT) return;
    if(rc || info.type!=LFS_TYPE_REG || info.size>=sizeof(text) ||
       !read_file("/state/nes.json",text,sizeof(text)-1,actual)) {writable_config=false; return;}
    text[actual]=0;
    cJSON *root=cJSON_ParseWithOpts(text,nullptr,true);
    if(!cJSON_IsObject(root)) {cJSON_Delete(root); writable_config=false; return;}
    auto *id=cJSON_GetObjectItemCaseSensitive(root,"controller_id");
    auto *level=cJSON_GetObjectItemCaseSensitive(root,"brightness");
    auto *swap=cJSON_GetObjectItemCaseSensitive(root,"swap_ab");
    auto *palette=cJSON_GetObjectItemCaseSensitive(root,"nes_palette");
    auto *background=cJSON_GetObjectItemCaseSensitive(root,"menu_background");
    auto *position=cJSON_GetObjectItemCaseSensitive(root,"game_position");
    auto *fps=cJSON_GetObjectItemCaseSensitive(root,"fps_monitor");
    auto *sides=cJSON_GetObjectItemCaseSensitive(root,"side_backgrounds");
    auto game_position=presentation::Position::Center;
    auto selected_palette=presentation::Palette::Current;
    auto *idle=cJSON_GetObjectItemCaseSensitive(root,"menu_sleep_seconds");
    auto *ble=cJSON_GetObjectItemCaseSensitive(root,"ble_enabled");
    auto *serial=cJSON_GetObjectItemCaseSensitive(root,"serial_enabled");
    auto *peer=cJSON_GetObjectItemCaseSensitive(root,"ble_peer");
    auto *random=cJSON_GetObjectItemCaseSensitive(root,"ble_random");
    auto *uuid=cJSON_GetObjectItemCaseSensitive(root,"ble_scan_uuid");
    uint8_t address[6];
    uint16_t scan_uuid=config.ble_scan_uuid;
    if((id && (!cJSON_IsNumber(id) || id->valuedouble!=id->valueint || id->valueint<1 || id->valueint>65535)) ||
       (position && (!cJSON_IsString(position) || !presentation::parse_position(position->valuestring,game_position))) ||
       (fps && !cJSON_IsBool(fps)) || (sides && !cJSON_IsObject(sides)) ||
       (level && (!cJSON_IsNumber(level) || level->valuedouble!=level->valueint || level->valueint<1 || level->valueint>100)) ||
       (swap && !cJSON_IsBool(swap)) ||
       (palette && (!cJSON_IsString(palette) || !presentation::parse_palette(palette->valuestring,selected_palette))) ||
       (background && (!cJSON_IsString(background) ||
        (background->valuestring[0] && !background::valid_path(background->valuestring)))) ||
       (idle && (!cJSON_IsNumber(idle) || idle->valuedouble!=idle->valueint || idle->valueint<0 || idle->valueint>86400)) ||
       (serial && !cJSON_IsBool(serial)) ||
       (ble && !cJSON_IsBool(ble)) || (random && !cJSON_IsBool(random)) ||
       (uuid && (!cJSON_IsString(uuid) || !bluetooth::uuid16(uuid->valuestring,scan_uuid))) ||
       (peer && (!cJSON_IsString(peer) || (peer->valuestring[0] && !bluetooth::address(peer->valuestring,address))))) {
        writable_config=false; cJSON_Delete(root); return;
    }
    for(unsigned i=0;sides && i<3;++i) {
        const auto p=presentation::Position(i);
        auto *value=cJSON_GetObjectItemCaseSensitive(sides,presentation::position_id(p));
        if(value && !cJSON_IsString(value)) {
            writable_config=false;cJSON_Delete(root);return;
        }
    }
    bluetooth::Policy policy;
    auto *mode=cJSON_GetObjectItemCaseSensitive(root,"ble_mode");
    auto *list=cJSON_GetObjectItemCaseSensitive(root,"ble_whitelist");
    auto *preferred=cJSON_GetObjectItemCaseSensitive(root,"ble_preferred");
    bool valid=(!mode || (cJSON_IsString(mode) &&
        (!std::strcmp(mode->valuestring,"auto") || !std::strcmp(mode->valuestring,"whitelist")))) &&
        (!list || (cJSON_IsArray(list) && cJSON_GetArraySize(list)<=int(bluetooth::Policy::Capacity)));
    if(valid && mode) policy.mode=!std::strcmp(mode->valuestring,"whitelist")?
        bluetooth::Mode::WhitelistOnly:bluetooth::Mode::AutoCompatible;
    if(valid && list) {
        cJSON *item=nullptr;
        cJSON_ArrayForEach(item,list) {
            auto *mac=cJSON_GetObjectItemCaseSensitive(item,"mac");
            auto *type=cJSON_GetObjectItemCaseSensitive(item,"random");
            bluetooth::Peer key;
            if(!cJSON_IsObject(item) || !cJSON_IsString(mac) || !cJSON_IsBool(type) ||
               !bluetooth::address(mac->valuestring,key.address)) {valid=false; break;}
            key.random=cJSON_IsTrue(type);
            if(policy.find(key)>=0 || !policy.add(key)) {valid=false; break;}
        }
    }
    // Migrate only the old /state fields; the former FAT override is not read.
    if(valid && !mode && !list && peer && peer->valuestring[0]) {
        bluetooth::Peer key; bluetooth::address(peer->valuestring,key.address);
        key.random=random && cJSON_IsTrue(random);
        policy.add(key); policy.preferred=0; policy.mode=bluetooth::Mode::WhitelistOnly;
    }
    if(preferred) {
        if(!cJSON_IsNumber(preferred) || preferred->valuedouble!=preferred->valueint ||
           (preferred->valueint!=-1 && (preferred->valueint<0 || preferred->valueint>=policy.count))) valid=false;
        else policy.preferred=preferred->valueint<0?255:uint8_t(preferred->valueint);
    }
    if(!valid) {writable_config=false; cJSON_Delete(root); return;}
    if(id) config.controller=id->valueint;
    if(level) config.brightness=level->valueint;
    if(swap) config.swap_ab=cJSON_IsTrue(swap);
    config.palette=selected_palette;
    if(background) std::snprintf(config.menu_background,sizeof config.menu_background,"%s",background->valuestring);
    config.game_position=game_position;
    if(fps) config.fps_monitor=cJSON_IsTrue(fps);
    std::memset(config.side_backgrounds,0,sizeof config.side_backgrounds);
    for(unsigned i=0;sides && i<3;++i) {
        const auto *value=cJSON_GetObjectItemCaseSensitive(sides,presentation::position_id(presentation::Position(i)));
        // Unusable artwork paths fall back to black without disabling settings saves.
        if(value && side_background::valid_path(presentation::Position(i),value->valuestring))
            std::snprintf(config.side_backgrounds[i],sizeof config.side_backgrounds[i],"%s",value->valuestring);
    }
    if(idle) config.menu_sleep_seconds=idle->valueint;
    if(ble) config.ble_enabled=cJSON_IsTrue(ble);
    if(serial) config.serial_enabled=cJSON_IsTrue(serial);
    config.ble=policy;
    config.ble_scan_uuid=scan_uuid;
    cJSON_Delete(root);
}
static void init_state() {
    if(state_mounted) return;
    lc.read=state_read; lc.prog=state_prog; lc.erase=state_erase; lc.sync=state_sync;
    lc.read_size=lc.prog_size=lc.cache_size=256;
    lc.block_size=4096; lc.block_count=kState.bytes/4096; lc.block_cycles=500;
    lc.lookahead_size=sizeof(lookahead); lc.lookahead_buffer=lookahead;
    lc.read_buffer=read_cache; lc.prog_buffer=prog_cache;
    state_mounted=lfs_mount(&lfs,&lc)==0;
}
bool init_disk() {
    if(disk_display::active()) init_state();
    flashfat::disk.read=read_system;
    if(flashfat::disk.probe()) {
        flashfat::disk.runtime_owned=true;
        fat_mounted=f_mount(&fat,"0:",1)==FR_OK;
    }
    return fat_mounted;
}
void init(bool use_state) {
    init_disk();
    rom.mount(reinterpret_cast<const void *>(kXipBase+kRom.offset),kRom.bytes);
    if(use_state) {
        init_state();
        if(state_mounted) load_config();
    }
    if(!fat_mounted) std::snprintf(error,sizeof(error),"Invalid /system FAT; no format performed");
}
bool mounted() {return fat_mounted;}
bool state_ready() {return state_mounted && writable_config;}
bool state_available() {return state_mounted;}
bool indexed() {return index_mode;}
uint32_t generation() {return storage_generation;}

static uint16_t index_u16(size_t at) {
    return uint16_t(index_data[at]) | uint16_t(index_data[at+1])<<8;
}
static uint32_t index_u32(size_t at) {
    return uint32_t(index_data[at]) | uint32_t(index_data[at+1])<<8 |
        uint32_t(index_data[at+2])<<16 | uint32_t(index_data[at+3])<<24;
}
static bool ascii_printable(const char *s,bool multiline) {
    for(;*s;++s) {
        const unsigned c=static_cast<unsigned char>(*s);
        if(c<32 || c>=127) {
            if(!(multiline && c=='\n')) return false;
        }
    }
    return true;
}
static bool valid_rom_leaf(const char *name) {
    const size_t n=std::strlen(name);
    if(!n || n>255 || !ascii_printable(name,false) || n<4 ||
       strcasecmp(name+n-4,".nes") || std::strstr(name,"..")) return false;
    for(const char *p=name;*p;++p)
        if(std::strchr("/\\:\"<>|?*",*p)) return false;
    return true;
}
static const char *index_string(uint32_t offset,bool multiline,bool required) {
    if(index_strings>=index_file_bytes || offset>=index_file_bytes-index_strings ||
       (offset && index_data[index_strings+offset-1]!=0)) return nullptr;
    const char *s=reinterpret_cast<const char *>(index_data+index_strings+offset);
    const size_t remaining=index_file_bytes-index_strings-offset;
    if(required && !*s) return nullptr;
    for(size_t i=0;i<remaining;++i) {
        if(!s[i]) return s;
        if(i%4096==0) tufty_service();
        const unsigned c=static_cast<unsigned char>(s[i]);
        if((c<32 || c>=127) && !(multiline && c=='\n')) return nullptr;
    }
    return nullptr;
}
static uint32_t index_crc(size_t bytes) {
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<bytes;++i) {
        if(i%4096==0) tufty_service();
        const uint8_t value=(i>=20 && i<24)?0:index_data[i];
        crc^=value;
        for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return crc^0xffffffffu;
}
static bool decode_index(size_t bytes) {
    index_mode=false; index_strings=0; index_file_bytes=0;
    if(bytes<32 || bytes>sizeof index_data) return false;
    if(std::memcmp(index_data,"GIDX",4) || index_u16(4)!=1 || index_u16(6)!=32 ||
       index_u16(10)!=16 || index_data[30]!=1 || index_u32(24)!=0 ||
       index_data[28]!=58 || index_data[29]!=80 || index_data[31]!=0) return false;
    const unsigned games=index_u16(8);
    const size_t records=32+size_t(games)*16, strings=index_u32(12), file_size=index_u32(16);
    if(games>kMaxRoms || strings!=records || file_size!=bytes || strings>=bytes || index_data[strings]!=0 ||
       index_crc(bytes)!=index_u32(20)) return false;
    index_strings=static_cast<uint32_t>(strings); index_file_bytes=static_cast<uint32_t>(bytes);
    for(unsigned i=0;i<games;++i) {
        const size_t at=32+size_t(i)*16;
        const uint32_t name_offset=index_u32(at), title_offset=index_u32(at+4), desc_offset=index_u32(at+8), flags=index_u32(at+12);
        const char *name=index_string(name_offset,false,true), *title_text=index_string(title_offset,false,true);
        const char *desc=index_string(desc_offset,true,false);
        if(!name || !title_text || !desc || !valid_rom_leaf(name) ||
           (flags&~3u) || bool(flags&2u)!=bool(*desc) || (!*desc && desc_offset!=0)) return false;
        for(unsigned prior=0;prior<i;++prior) if(!strcasecmp(table[prior].name,name)) return false;
        std::snprintf(table[i].name,sizeof(table[i].name),"%s",name);
        table[i].bytes=0; table[i].title_offset=title_offset; table[i].description_offset=desc_offset; table[i].flags=flags;
    }
    count=games; index_mode=true; return true;
}
bool load_catalog() {
    size_t bytes=0;
    count=0; index_mode=false; ++storage_generation;
    return read_file("/state/games.idx",index_data,sizeof index_data,bytes) && decode_index(bytes);
}
bool install_catalog(const void *data,size_t bytes) {
    if(bytes>sizeof index_data) return false;
    std::memcpy(index_data,data,bytes); ++storage_generation;
    return decode_index(bytes);
}
uint32_t catalog_crc() {return index_mode?index_u32(20):0;}
size_t copy_catalog(void *data,size_t capacity) {
    if(!index_mode || capacity<index_file_bytes) return 0;
    std::memcpy(data,index_data,index_file_bytes); return index_file_bytes;
}
void *library_scratch() {return rom_data;}
const char *title(size_t index) {
    if(index>=count || !index_mode) return nullptr;
    return reinterpret_cast<const char *>(index_data+index_strings+table[index].title_offset);
}
const char *description(size_t index) {
    if(index>=count || !index_mode) return nullptr;
    return reinterpret_cast<const char *>(index_data+index_strings+table[index].description_offset);
}
bool has_cover(size_t index) {
    return index<count && (index_mode ? (table[index].flags&1u)!=0 : true);
}
static bool canonical_path(const char *path) {
    if(!path || *path!='/') return false;
    for(const char *p=path+1;*p;) {
        const char *start=p;
        while(*p && *p!='/') {
            if(static_cast<unsigned char>(*p)<32 || *p==':' || *p=='\\') return false;
            ++p;
        }
        const size_t n=p-start;
        if(!n || (n==1 && start[0]=='.') || (n==2 && start[0]=='.' && start[1]=='.')) return false;
        if(*p) ++p;
    }
    return true;
}
static bool under(const char *path,const char *root) {
    const size_t n=std::strlen(root);return !std::strncmp(path,root,n) && (!path[n] || path[n]=='/');
}
bool inspect_path(const char *path,PathInfo &info) {
    info={};if(!canonical_path(path)) return false;
    if(!std::strcmp(path,"/")) {info.directory=true;return true;}
    if(under(path,"/rom")) {
        tufty::storage::RomView view;
        if(rom.find(path,view)!=tufty::storage::RomResult::Ok) return false;
        info={view.directory,view.bytes};return true;
    }
    if(under(path,"/system") && fat_mounted) {
        if(!path[7]) {info.directory=true;return true;}
        FILINFO stat{};if(f_stat(path+7,&stat)!=FR_OK) return false;
        info={bool(stat.fattrib&AM_DIR),size_t(stat.fsize)};return true;
    }
    if(under(path,"/state") && state_mounted) {
        lfs_info stat{};if(lfs_stat(&lfs,path,&stat)) return false;
        info={stat.type==LFS_TYPE_DIR,stat.size};return true;
    }
    return false;
}
bool list_directory(const char *path,DirectoryVisitor visitor,void *context) {
    if(!canonical_path(path) || !visitor) return false;
    if(!std::strcmp(path,"/")) return visitor("rom",true,0,context) && visitor("system",true,0,context) && visitor("state",true,0,context);
    if(under(path,"/rom")) {
        using tufty::storage::RomResult;
        tufty::storage::RomView view;
        if(rom.find(path,view)!=RomResult::Ok || !view.directory) return false;
        uint32_t cursor=0;tufty::storage::RomEntry entry;
        for(;;) {
            const auto rc=rom.next(view,cursor,entry);
            if(rc==RomResult::End) return true;
            if(rc!=RomResult::Ok || entry.name_bytes>=256) return false;
            char name[256];std::memcpy(name,entry.name,entry.name_bytes);name[entry.name_bytes]=0;
            if(!visitor(name,entry.view.directory,entry.view.bytes,context)) return false;
            tufty_service();
        }
    }
    if(under(path,"/system") && fat_mounted) {
        SystemReader reader;DIR dir{};
        if(f_opendir(&dir,path[7]?path+7:"/")!=FR_OK) return false;
        FILINFO info{};FRESULT rc;bool ok=true;
        while((rc=f_readdir(&dir,&info))==FR_OK && info.fname[0]) {
            if(std::strcmp(info.fname,".") && std::strcmp(info.fname,"..") &&
               !visitor(info.fname,bool(info.fattrib&AM_DIR),info.fsize,context)) {ok=false;break;}
            tufty_service();
        }
        return f_closedir(&dir)==FR_OK && rc==FR_OK && ok;
    }
    if(under(path,"/state") && state_mounted) {
        lfs_dir_t dir{};if(lfs_dir_open(&lfs,&dir,path)) return false;
        lfs_info info{};int rc;bool ok=true;
        while((rc=lfs_dir_read(&lfs,&dir,&info))>0) {
            if(std::strcmp(info.name,".") && std::strcmp(info.name,"..") &&
               !visitor(info.name,info.type==LFS_TYPE_DIR,info.size,context)) {ok=false;break;}
            tufty_service();
        }
        return lfs_dir_close(&lfs,&dir)==0 && rc>=0 && ok;
    }
    return false;
}
static bool read_file_impl(const char *path,void *out,size_t capacity,size_t &actual,bool prefix) {
    actual=0;
    if(!canonical_path(path) || !out) return false;
    if(!std::strncmp(path,"/rom/",5)) {
        tufty::storage::RomView view;
        if(rom.find(path,view)!=tufty::storage::RomResult::Ok || view.directory || (!prefix && view.bytes>capacity)) return false;
        actual=std::min<size_t>(view.bytes,capacity);std::memcpy(out,view.data,actual);return true;
    }
    if(!std::strncmp(path,"/system/",8)) {
        if(!fat_mounted) return false;
        SystemReader reader;
        FIL f{}; if(f_open(&f,path+7,FA_READ)!=FR_OK) return false;
        bool ok=prefix || f_size(&f)<=capacity;
        const size_t bytes=std::min<size_t>(f_size(&f),capacity);
        while(ok && actual<bytes) {
            UINT got=0; UINT chunk=std::min<size_t>(4096,bytes-actual);
            ok=f_read(&f,static_cast<uint8_t *>(out)+actual,chunk,&got)==FR_OK && got==chunk;
            actual+=got; tufty_service();
        }
        ok=f_close(&f)==FR_OK && ok; return ok;
    }
    if(!std::strncmp(path,"/state/",7) && state_mounted) {
        uint8_t cache[256]; lfs_file_config fc{}; fc.buffer=cache; lfs_file_t f{};
        if(lfs_file_opencfg(&lfs,&f,path,LFS_O_RDONLY,&fc)) return false;
        lfs_soff_t n=lfs_file_size(&lfs,&f);
        bool ok=n>=0 && (prefix || static_cast<size_t>(n)<=capacity);
        if(ok) {const size_t bytes=std::min<size_t>(n,capacity);lfs_ssize_t got=lfs_file_read(&lfs,&f,out,bytes);ok=got>=0 && size_t(got)==bytes;if(ok)actual=got;}
        return lfs_file_close(&lfs,&f)==0 && ok;
    }
    return false;
}
bool read_file(const char *path,void *out,size_t capacity,size_t &actual) {
    return read_file_impl(path,out,capacity,actual,false);
}
bool read_file_prefix(const char *path,void *out,size_t capacity,size_t &actual) {
    return read_file_impl(path,out,capacity,actual,true);
}
FileStatus file_size(const char *path,size_t &bytes) {
    bytes=0;
    if(!std::strncmp(path,"/system/",8) && fat_mounted) {
        FILINFO info{}; const auto rc=f_stat(path+7,&info);
        if(rc==FR_NO_FILE || rc==FR_NO_PATH) return FileStatus::Missing;
        if(rc!=FR_OK || (info.fattrib&AM_DIR)) return FileStatus::Error;
        bytes=info.fsize; return FileStatus::File;
    }
    if(!std::strncmp(path,"/state/",7) && state_mounted) {
        lfs_info info{}; const auto rc=lfs_stat(&lfs,path,&info);
        if(rc==LFS_ERR_NOENT) return FileStatus::Missing;
        if(rc || info.type!=LFS_TYPE_REG) return FileStatus::Error;
        bytes=info.size; return FileStatus::File;
    }
    return FileStatus::Error;
}
struct FlashPause {
    bool own=!disk_display::active() && !display::paused();
    FlashPause() {if(own) display::pause(true);}
    ~FlashPause() {if(own) display::pause(false);}
};
bool reset_state() {
    if(disk_display::active()) return false;
    init_state();
    FlashPause pause;
    if(state_mounted) {
        if(lfs_unmount(&lfs)) return false;
        state_mounted=false;
    }
    const bool formatted=lfs_format(&lfs,&lc)==0;
    state_mounted=lfs_mount(&lfs,&lc)==0;
    writable_config=formatted && state_mounted;
    return writable_config;
}
bool write_state(const char *path,const void *data,size_t bytes,Progress progress) {
    if(!state_mounted) return false;
    // One library operation at a time; configuration has its own temporary file.
    constexpr const char *temporary="/state/library.tmp";
    uint8_t cache[256]; lfs_file_config fc{}; fc.buffer=cache; lfs_file_t f{};
    {
        FlashPause pause;
        const int rc=lfs_mkdir(&lfs,"/state");
        if(rc && rc!=LFS_ERR_EXIST) return false;
        if(lfs_file_opencfg(&lfs,&f,temporary,LFS_O_RDWR|LFS_O_CREAT|LFS_O_TRUNC,&fc)) return false;
    }
    bool ok=true;
    const auto *source=static_cast<const uint8_t *>(data);
    for(size_t at=0;ok && at<bytes;at+=4096) {
        const size_t n=std::min<size_t>(4096,bytes-at);
        {FlashPause pause; ok=lfs_file_write(&lfs,&f,source+at,n)==lfs_ssize_t(n);}
        if(!disk_display::active()) tufty_service();
        if(progress) progress();
    }
    {FlashPause pause; if(ok) ok=lfs_file_sync(&lfs,&f)==0;}
    if(ok) ok=lfs_file_rewind(&lfs,&f)==0;
    uint8_t verify[256];
    for(size_t at=0;ok && at<bytes;at+=sizeof verify) {
        const size_t n=std::min(sizeof verify,bytes-at);
        ok=lfs_file_read(&lfs,&f,verify,n)==lfs_ssize_t(n) && !std::memcmp(verify,source+at,n);
        if(progress && at%4096==0) progress();
    }
    {FlashPause pause;
        ok=lfs_file_close(&lfs,&f)==0 && ok;
        if(ok) ok=lfs_rename(&lfs,temporary,path)==0;
    }
    return ok;
}
bool remove_state(const char *path) {
    if(!state_mounted) return false;
    FlashPause pause;
    const int rc=lfs_remove(&lfs,path);
    return rc==0 || rc==LFS_ERR_NOENT;
}
bool mark_library_changed() {
    size_t bytes=0;
    const auto status=file_size("/state/library.dirty",bytes);
    if(status==FileStatus::File) return true;
    if(status==FileStatus::Error) return false;
    const uint8_t dirty=1;
    return write_state("/state/library.dirty",&dirty,1);
}
bool visit_files(const char *directory,Visitor visitor,void *context) {
    if(!directory || !visitor) return false;
    if(!std::strncmp(directory,"/rom/",5)) {
        tufty::storage::RomView view;
        const auto result=rom.find(directory,view);
        using tufty::storage::RomResult;
        if(result==RomResult::NotFound || result==RomResult::NotReady) return true;
        if(result!=RomResult::Ok || !view.directory) return false;
        uint32_t cursor=0; tufty::storage::RomEntry entry;
        for(;;) {
            const auto rc=rom.next(view,cursor,entry);
            if(rc==RomResult::End) return true;
            if(rc!=RomResult::Ok) return false;
            if(!entry.view.directory && entry.name_bytes<256) {
                char name[256]; std::memcpy(name,entry.name,entry.name_bytes); name[entry.name_bytes]=0;
                visitor(name,entry.view.bytes,context);
            }
            tufty_service();
        }
    }
    if(std::strncmp(directory,"/system/",8) || std::strstr(directory,"..") ||
       std::strchr(directory,':') || std::strchr(directory,'\\')) return false;
    if(!fat_mounted) return false;
    SystemReader reader;
    DIR dir{};
    FRESULT rc=f_opendir(&dir,directory+7);
    if(rc==FR_NO_PATH || rc==FR_NO_FILE) return true;
    if(rc!=FR_OK) return false;
    FILINFO info{};
    while((rc=f_readdir(&dir,&info))==FR_OK && info.fname[0]) {
        if(!(info.fattrib&(AM_DIR|AM_HID|AM_SYS))) visitor(info.fname,info.fsize,context);
        tufty_service();
    }
    return f_closedir(&dir)==FR_OK && rc==FR_OK;
}
bool visit_roms(Visitor visitor,void *context) {return visit_files("/system/roms",visitor,context);}
bool load(size_t index,const uint8_t *&data,size_t &bytes) {
    data=nullptr; bytes=0;
    if(index>=count) {
        std::snprintf(error,sizeof(error),"ROM must be 16 bytes to 4 MiB"); return false;
    }
    char path[272]; std::snprintf(path,sizeof(path),"/system/roms/%s",table[index].name);
    FILINFO info{};
    const auto stat=f_stat(path+7,&info);
    if(stat!=FR_OK) {
        std::snprintf(error,sizeof(error),"%s",
            stat==FR_NO_FILE || stat==FR_NO_PATH?"ROM missing":"Read failed");
        return false;
    }
    if(info.fsize<16 || info.fsize>kMaxRomBytes) {
        std::snprintf(error,sizeof(error),"Invalid ROM size"); return false;
    }
    if(!read_file(path,rom_data,sizeof(rom_data),bytes)) {
        std::snprintf(error,sizeof(error),"Read failed"); return false;
    }
    if(bytes<16 || bytes>kMaxRomBytes) {
        std::snprintf(error,sizeof(error),"ROM must be 16 bytes to 4 MiB"); return false;
    }
    table[index].bytes=static_cast<uint32_t>(bytes);
    data=rom_data; return true;
}
bool save() {
    if(!state_ready()) return false;
    char uuid[7]; std::snprintf(uuid,sizeof(uuid),"0x%04X",config.ble_scan_uuid);
    cJSON *root=cJSON_CreateObject(); if(!root) return false;
    bool ok=cJSON_AddNumberToObject(root,"controller_id",config.controller) &&
        cJSON_AddNumberToObject(root,"brightness",config.brightness) &&
        cJSON_AddBoolToObject(root,"swap_ab",config.swap_ab) &&
        cJSON_AddStringToObject(root,"nes_palette",presentation::palette_id(config.palette)) &&
        cJSON_AddStringToObject(root,"menu_background",config.menu_background) &&
        cJSON_AddStringToObject(root,"game_position",presentation::position_id(config.game_position)) &&
        cJSON_AddBoolToObject(root,"fps_monitor",config.fps_monitor) &&
        cJSON_AddNumberToObject(root,"menu_sleep_seconds",config.menu_sleep_seconds) &&
        cJSON_AddBoolToObject(root,"serial_enabled",config.serial_enabled) &&
        cJSON_AddBoolToObject(root,"ble_enabled",config.ble_enabled) &&
        cJSON_AddStringToObject(root,"ble_mode",config.ble.mode==bluetooth::Mode::WhitelistOnly?"whitelist":"auto") &&
        cJSON_AddNumberToObject(root,"ble_preferred",config.ble.preferred==255?-1:config.ble.preferred) &&
        cJSON_AddStringToObject(root,"ble_scan_uuid",uuid);
    auto *sides=cJSON_AddObjectToObject(root,"side_backgrounds");
    ok=ok && sides;
    for(unsigned i=0;ok && i<3;++i)
        ok=cJSON_AddStringToObject(sides,presentation::position_id(presentation::Position(i)),config.side_backgrounds[i]);
    auto *list=cJSON_AddArrayToObject(root,"ble_whitelist");
    ok=ok && list;
    for(unsigned i=0;ok && i<config.ble.count;++i) {
        auto *item=cJSON_CreateObject();
        if(!item) {ok=false; break;}
        char mac[18]; config.ble.peers[i].text(mac);
        ok=cJSON_AddStringToObject(item,"mac",mac) &&
            cJSON_AddBoolToObject(item,"random",config.ble.peers[i].random);
        if(!ok || !cJSON_AddItemToArray(list,item)) {cJSON_Delete(item); ok=false;}
    }
    char *json=ok?cJSON_PrintUnformatted(root):nullptr; cJSON_Delete(root);
    if(!json) return false;
    display::pause(true);
    int rc=lfs_mkdir(&lfs,"/state"); ok=rc==0 || rc==LFS_ERR_EXIST;
    uint8_t cache[256]; lfs_file_config fc{}; fc.buffer=cache; lfs_file_t f{};
    if(ok) {
        rc=lfs_file_opencfg(&lfs,&f,"/state/nes.tmp",LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC,&fc);
        ok=rc==0;
        if(ok) {
            size_t n=std::strlen(json);
            ok=lfs_file_write(&lfs,&f,json,n)==static_cast<lfs_ssize_t>(n);
            if(ok) ok=lfs_file_sync(&lfs,&f)==0;
            ok=lfs_file_close(&lfs,&f)==0 && ok;
            if(ok) ok=lfs_rename(&lfs,"/state/nes.tmp","/state/nes.json")==0;
        }
    }
    display::pause(false); cJSON_free(json); return ok;
}
bool unmount_system() {
    if(system_readers) return false;
    if(fat_mounted && f_mount(nullptr,"0:",0)!=FR_OK) return false;
    fat_mounted=false; flashfat::disk.runtime_owned=false; return true;
}
bool replace_system_sector(uint32_t at,const uint8_t *data) {
    if((!disk_display::active() && !display::paused()) || fat_mounted || flashfat::disk.runtime_owned || at%4096 || !kSystem.contains(at,4096)) return false;
    Operation op{kSystem.offset+at,4096,data,true};
    return execute(op) && !std::memcmp(reinterpret_cast<const void *>(kXipBase+op.at),data,4096);
}
}
