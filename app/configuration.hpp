#pragma once
#include "storage.hpp"
#include "usb.hpp"
#include "display.hpp"

// Core0's persistence baseline. No snapshots are copied back over live settings.
namespace configuration {
enum class SaveResult { Unchanged, Saved, Unavailable, Failed };
inline storage::Config baseline;
inline bool trusted=false;
inline bool equal(const storage::Config &a,const storage::Config &b) {
    if(a.controller!=b.controller || a.brightness!=b.brightness || a.swap_ab!=b.swap_ab ||
       a.serial_enabled!=b.serial_enabled || a.ble_enabled!=b.ble_enabled ||
       a.ble_scan_uuid!=b.ble_scan_uuid || a.menu_sleep_seconds!=b.menu_sleep_seconds ||
       a.ble.mode!=b.ble.mode || a.ble.count!=b.ble.count || a.ble.preferred!=b.ble.preferred) return false;
    for(unsigned i=0;i<a.ble.count;++i) if(!(a.ble.peers[i]==b.ble.peers[i])) return false;
    return true;
}
inline void init() {baseline=storage::config; trusted=storage::state_ready(); usb::dirty=false;}
inline bool changed() {return !equal(baseline,storage::config);}
inline void sync() {usb::dirty=changed();}
inline bool available() {return trusted && storage::state_ready();}
inline SaveResult save() {
    usb::save_requested=false; sync();
    if(!available()) return SaveResult::Unavailable;
    if(!changed()) return SaveResult::Unchanged;
    const auto written=storage::config;
    if(!storage::save()) return SaveResult::Failed;
    baseline=written; sync(); return SaveResult::Saved;
}
inline void brightness(unsigned value) {
    storage::config.brightness=uint8_t(value); display::brightness(uint8_t(value)); sync();
}
inline void swap(bool enabled) {storage::config.swap_ab=enabled; sync();}
inline void idle(uint32_t seconds) {storage::config.menu_sleep_seconds=seconds; sync();}
}
