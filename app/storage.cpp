#include "storage.hpp"
#include "fat_disk.hpp"
#include "rom_store.hpp"
#include "display.hpp"
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
static uint8_t __uninitialized_psram("rom_data") rom_data[kMaxRomBytes];
Entry *entries=table;
size_t count=0;
static FATFS fat;
static bool fat_mounted=false, state_mounted=false, writable_config=true;
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
    if(!state_range(block,at,bytes) || at%256 || bytes%256 || !display::paused()) return LFS_ERR_INVAL;
    alignas(4) uint8_t page[256];
    for(uint32_t n=0;n<bytes;n+=256) {
        std::memcpy(page,static_cast<const uint8_t *>(in)+n,256);
        Operation op{kState.offset+block*4096+at+n,256,page,false};
        if(!execute(op) || std::memcmp(reinterpret_cast<const void *>(kXipBase+op.at),page,256)) return LFS_ERR_IO;
    }
    return 0;
}
static int state_erase(const lfs_config *,lfs_block_t block) {
    if(!state_range(block,0,4096) || !display::paused()) return LFS_ERR_INVAL;
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
    auto *last=cJSON_GetObjectItemCaseSensitive(root,"last_rom");
    auto *idle=cJSON_GetObjectItemCaseSensitive(root,"menu_sleep_seconds");
    auto *ble=cJSON_GetObjectItemCaseSensitive(root,"ble_enabled");
    auto *serial=cJSON_GetObjectItemCaseSensitive(root,"serial_enabled");
    auto *peer=cJSON_GetObjectItemCaseSensitive(root,"ble_peer");
    auto *random=cJSON_GetObjectItemCaseSensitive(root,"ble_random");
    auto *uuid=cJSON_GetObjectItemCaseSensitive(root,"ble_scan_uuid");
    uint8_t address[6];
    uint16_t scan_uuid=config.ble_scan_uuid;
    if((id && (!cJSON_IsNumber(id) || id->valuedouble!=id->valueint || id->valueint<1 || id->valueint>65535)) ||
       (level && (!cJSON_IsNumber(level) || level->valuedouble!=level->valueint || level->valueint<1 || level->valueint>100)) ||
       (swap && !cJSON_IsBool(swap)) || (last && (!cJSON_IsString(last) || std::strlen(last->valuestring)>255)) ||
       (idle && (!cJSON_IsNumber(idle) || idle->valuedouble!=idle->valueint || idle->valueint<0 || idle->valueint>86400)) ||
       (serial && !cJSON_IsBool(serial)) ||
       (ble && !cJSON_IsBool(ble)) || (random && !cJSON_IsBool(random)) ||
       (uuid && (!cJSON_IsString(uuid) || !bluetooth::uuid16(uuid->valuestring,scan_uuid))) ||
       (peer && (!cJSON_IsString(peer) || (peer->valuestring[0] && !bluetooth::address(peer->valuestring,address))))) {
        writable_config=false; cJSON_Delete(root); return;
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
    if(last) std::snprintf(config.last_rom,sizeof(config.last_rom),"%s",last->valuestring);
    if(idle) config.menu_sleep_seconds=idle->valueint;
    if(ble) config.ble_enabled=cJSON_IsTrue(ble);
    if(serial) config.serial_enabled=cJSON_IsTrue(serial);
    config.ble=policy;
    config.ble_scan_uuid=scan_uuid;
    cJSON_Delete(root);
}
void init(bool use_state) {
    flashfat::disk.read=read_system;
    if(flashfat::disk.probe()) {
        flashfat::disk.runtime_owned=true;
        fat_mounted=f_mount(&fat,"0:",1)==FR_OK;
    }
    rom.mount(reinterpret_cast<const void *>(kXipBase+kRom.offset),kRom.bytes);
    if(use_state) {
        lc.read=state_read; lc.prog=state_prog; lc.erase=state_erase; lc.sync=state_sync;
        lc.read_size=lc.prog_size=lc.cache_size=256;
        lc.block_size=4096; lc.block_count=kState.bytes/4096; lc.block_cycles=500;
        lc.lookahead_size=sizeof(lookahead); lc.lookahead_buffer=lookahead;
        lc.read_buffer=read_cache; lc.prog_buffer=prog_cache;
        state_mounted=lfs_mount(&lfs,&lc)==0;
        if(state_mounted) load_config();
    }
    if(!fat_mounted) std::snprintf(error,sizeof(error),"Invalid /system FAT; no format performed");
}
bool mounted() {return fat_mounted;}
bool state_ready() {return state_mounted && writable_config;}
bool read_file(const char *path,void *out,size_t capacity,size_t &actual) {
    actual=0;
    if(!path || !out || std::strstr(path,"..") || std::strchr(path,':') || std::strchr(path,'\\')) return false;
    if(!std::strncmp(path,"/rom/",5)) {
        tufty::storage::RomView view;
        if(rom.find(path,view)!=tufty::storage::RomResult::Ok || view.directory || view.bytes>capacity) return false;
        std::memcpy(out,view.data,view.bytes); actual=view.bytes; return true;
    }
    if(!std::strncmp(path,"/system/",8)) {
        if(!fat_mounted) return false;
        FIL f{}; if(f_open(&f,path+7,FA_READ)!=FR_OK) return false;
        bool ok=f_size(&f)<=capacity;
        while(ok && actual<f_size(&f)) {
            UINT got=0; UINT chunk=std::min<size_t>(4096,f_size(&f)-actual);
            ok=f_read(&f,static_cast<uint8_t *>(out)+actual,chunk,&got)==FR_OK && got==chunk;
            actual+=got; tufty_service();
        }
        ok=f_close(&f)==FR_OK && ok; return ok;
    }
    if(!std::strncmp(path,"/state/",7) && state_mounted) {
        uint8_t cache[256]; lfs_file_config fc{}; fc.buffer=cache; lfs_file_t f{};
        if(lfs_file_opencfg(&lfs,&f,path,LFS_O_RDONLY,&fc)) return false;
        lfs_soff_t n=lfs_file_size(&lfs,&f);
        bool ok=n>=0 && static_cast<size_t>(n)<=capacity;
        if(ok) {lfs_ssize_t got=lfs_file_read(&lfs,&f,out,n); ok=got==n; if(ok) actual=got;}
        return lfs_file_close(&lfs,&f)==0 && ok;
    }
    return false;
}
void scan() {
    count=0;
    if(!fat_mounted) return;
    error[0]=0;
    DIR dir{};
    FRESULT rc=f_opendir(&dir,"/roms");
    if(rc!=FR_OK) {std::snprintf(error,sizeof(error),"Copy .nes files to /system/roms"); return;}
    FILINFO info{};
    while((rc=f_readdir(&dir,&info))==FR_OK && info.fname[0]) {
        size_t n=std::strlen(info.fname);
        if(!(info.fattrib&AM_DIR) && n>4 && n<256 && !strcasecmp(info.fname+n-4,".nes")) {
            if(count==kMaxRoms) {std::printf("ROM list limit: 128 games\n"); break;}
            std::memcpy(table[count].name,info.fname,n+1);
            table[count++].bytes=info.fsize;
        }
        tufty_service();
    }
    f_closedir(&dir);
    if(rc!=FR_OK) std::snprintf(error,sizeof(error),"Directory read failed (%u)",unsigned(rc));
    std::sort(table,table+count,[](const Entry &a,const Entry &b){return strcasecmp(a.name,b.name)<0;});
    if(!count && !error[0]) std::snprintf(error,sizeof(error),"Copy .nes files to /system/roms");
}
bool load(size_t index,const uint8_t *&data,size_t &bytes) {
    data=nullptr; bytes=0;
    if(index>=count || table[index].bytes<16 || table[index].bytes>kMaxRomBytes) {
        std::snprintf(error,sizeof(error),"ROM must be 16 bytes to 4 MiB"); return false;
    }
    char path[272]; std::snprintf(path,sizeof(path),"/system/roms/%s",table[index].name);
    if(!read_file(path,rom_data,sizeof(rom_data),bytes)) {
        std::snprintf(error,sizeof(error),"ROM read failed"); return false;
    }
    data=rom_data; return true;
}
bool save() {
    if(!state_ready()) return false;
    char uuid[7]; std::snprintf(uuid,sizeof(uuid),"0x%04X",config.ble_scan_uuid);
    cJSON *root=cJSON_CreateObject(); if(!root) return false;
    bool ok=cJSON_AddNumberToObject(root,"controller_id",config.controller) &&
        cJSON_AddNumberToObject(root,"brightness",config.brightness) &&
        cJSON_AddBoolToObject(root,"swap_ab",config.swap_ab) &&
        cJSON_AddNumberToObject(root,"menu_sleep_seconds",config.menu_sleep_seconds) &&
        cJSON_AddBoolToObject(root,"serial_enabled",config.serial_enabled) &&
        cJSON_AddBoolToObject(root,"ble_enabled",config.ble_enabled) &&
        cJSON_AddStringToObject(root,"ble_mode",config.ble.mode==bluetooth::Mode::WhitelistOnly?"whitelist":"auto") &&
        cJSON_AddNumberToObject(root,"ble_preferred",config.ble.preferred==255?-1:config.ble.preferred) &&
        cJSON_AddStringToObject(root,"ble_scan_uuid",uuid) &&
        cJSON_AddStringToObject(root,"last_rom",config.last_rom);
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
    if(fat_mounted && f_mount(nullptr,"0:",0)!=FR_OK) return false;
    fat_mounted=false; flashfat::disk.runtime_owned=false; return true;
}
bool replace_system_sector(uint32_t at,const uint8_t *data) {
    if(!display::paused() || fat_mounted || flashfat::disk.runtime_owned || at%4096 || !kSystem.contains(at,4096)) return false;
    Operation op{kSystem.offset+at,4096,data,true};
    return execute(op) && !std::memcmp(reinterpret_cast<const void *>(kXipBase+op.at),data,4096);
}
}
