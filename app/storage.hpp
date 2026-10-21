#pragma once
#include <cstddef>
#include <cstdint>
#include "bluetooth_config.hpp"
namespace storage {
constexpr size_t kMaxRoms=128, kMaxRomBytes=4*1024*1024;
struct Entry {
    char name[256];
    uint32_t bytes;
    // Metadata offsets are relative to the validated games.idx string table.
    // They are zero when the directory-scan fallback is active.
    uint32_t title_offset=0, description_offset=0, flags=0;
};
struct Config {
    uint16_t controller=1;
    uint8_t brightness=80;
    bool swap_ab=false;
    bool serial_enabled=true;
    bool ble_enabled=true;
    bluetooth::Policy ble;
    uint16_t ble_scan_uuid=0x1812;
    uint32_t menu_sleep_seconds=300;
};
extern Config config;
extern Entry *entries;
extern size_t count;
extern char error[96];
bool indexed();
uint32_t generation();
const char *title(size_t index);
const char *description(size_t index);
bool has_cover(size_t index);
void init(bool state=true);
bool mounted();
// Maintenance scans may bypass an otherwise valid but outdated index.
void scan(bool directory_only=false);
bool load(size_t index,const uint8_t *&data,size_t &bytes);
bool read_file(const char *path,void *out,size_t capacity,size_t &actual);
bool save();
bool unmount_system();
bool replace_system_sector(uint32_t offset,const uint8_t *sram);
bool state_ready();
}
