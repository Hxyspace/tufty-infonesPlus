#pragma once
#include <cstddef>
#include <cstdint>
#include "bluetooth_config.hpp"
#include "presentation/nes_palette.hpp"
#include "background_path.hpp"
#include "side_background_path.hpp"
namespace storage {
constexpr size_t kMaxRoms=128, kMaxRomBytes=4*1024*1024;
struct Entry {
    char name[256];
    uint32_t bytes;
    // Metadata offsets are relative to the validated games.idx string table.
    uint32_t title_offset=0, description_offset=0, flags=0;
};
struct Config {
    uint16_t controller=1;
    uint8_t brightness=80;
    presentation::Palette palette=presentation::Palette::Current;
    char menu_background[background::path_bytes]{}; // Empty selects the built-in default.
    presentation::Position game_position=presentation::Position::Center;
    bool fps_monitor=false;
    char side_backgrounds[3][side_background::path_bytes]{}; // One choice for each position; empty is black.
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
// Disk boot validates System and mounts only State for the durable change flag.
bool init_disk();
bool mounted();
bool load_catalog();
bool install_catalog(const void *data,size_t bytes);
size_t copy_catalog(void *data,size_t capacity);
uint32_t catalog_crc();
// Library maintenance owns this scratch space only while no ROM is running.
void *library_scratch();
enum class FileStatus {Missing,File,Error};
FileStatus file_size(const char *path,size_t &bytes);
using Visitor=void (*)(const char *name,uint32_t bytes,void *context);
bool visit_roms(Visitor visitor,void *context);
bool visit_files(const char *directory,Visitor visitor,void *context);
using Progress=void (*)();
bool scan(bool force=false,Progress progress=nullptr);
bool write_state(const char *path,const void *data,size_t bytes,Progress progress=nullptr);
bool remove_state(const char *path);
bool mark_library_changed();
bool load(size_t index,const uint8_t *&data,size_t &bytes);
bool read_file(const char *path,void *out,size_t capacity,size_t &actual);
bool read_file_prefix(const char *path,void *out,size_t capacity,size_t &actual);
// Read-only namespace inspection. Paths must be canonical absolute paths.
struct PathInfo {bool directory=false; size_t bytes=0;};
bool inspect_path(const char *path,PathInfo &info);
using DirectoryVisitor=bool (*)(const char *name,bool directory,size_t bytes,void *context);
bool list_directory(const char *path,DirectoryVisitor visitor,void *context);
bool save();
// Reformat State only. The caller must reboot on success without saving live settings.
bool reset_state();
bool unmount_system();
bool replace_system_sector(uint32_t offset,const uint8_t *sram);
bool state_ready();
bool state_available();
}
