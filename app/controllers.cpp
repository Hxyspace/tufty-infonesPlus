#include "controllers.hpp"
#include "ble.hpp"
#include "usb.hpp"
#include "storage.hpp"
#include <cstdio>
#include <cstring>

namespace controllers {
namespace {
void dirty() {usb::dirty=storage::state_ready();}
void apply_bluetooth(bool reconnect=true) {
    const auto &cfg=storage::config;
    ble::configure({cfg.ble_enabled,cfg.ble_scan_uuid,cfg.ble,cfg.controller},reconnect);
}
bool parse_peer(const char *text,bluetooth::Peer &key) {
    char mac[18],type[7],tail;
    if(std::sscanf(text,"%17s %6s %c",mac,type,&tail)!=2 || !bluetooth::address(mac,key.address) ||
       (std::strcmp(type,"public") && std::strcmp(type,"random"))) return false;
    key.random=!std::strcmp(type,"random"); return true;
}
}
Settings settings() {
    const auto &cfg=storage::config;
    return {cfg.serial_enabled,cfg.ble_enabled,cfg.controller,cfg.ble};
}
void init() {
    // USB may have been serviced while mounting storage. Discard any session
    // negotiated with startup defaults before applying the persisted ID/switch.
    usb::pad.reset(true); usb::pad.id=storage::config.controller;
    input::hub.attach(input::Source::Serial,usb::pad.input);
    input::hub.attach(input::Source::Ble,ble::input_receiver());
    input::hub.enable(input::Source::Serial,storage::config.serial_enabled);
    input::hub.enable(input::Source::Ble,storage::config.ble_enabled);
    if(storage::config.serial_enabled) usb::pad.announce();
    apply_bluetooth(false);
}
void set_controller_id(uint16_t id) {
    if(!id || storage::config.controller==id) return;
    storage::config.controller=id; usb::pad.reset(true); usb::pad.id=id;
    if(storage::config.serial_enabled) usb::pad.announce();
    apply_bluetooth(); dirty();
}
void set_enabled(input::Source source,bool enabled) {
    if(source==input::Source::Serial) {
        if(storage::config.serial_enabled==enabled) return;
        storage::config.serial_enabled=enabled;
        usb::pad.reset(true);
        if(enabled) usb::pad.announce();
    } else if(source==input::Source::Ble) {
        if(storage::config.ble_enabled==enabled) return;
        storage::config.ble_enabled=enabled; apply_bluetooth();
    } else return;
    input::hub.enable(source,enabled); dirty();
}
bool whitelist_add(const bluetooth::Peer &peer,bool preferred) {
    auto &policy=storage::config.ble;
    if(!policy.add(peer)) return false;
    if(preferred) policy.preferred=uint8_t(policy.find(peer));
    dirty(); apply_bluetooth(false); return true;
}
bool whitelist_remove(const bluetooth::Peer &peer) {
    if(!storage::config.ble.remove(peer)) return false;
    dirty(); apply_bluetooth(); return true;
}
void whitelist_clear() {
    auto &policy=storage::config.ble;
    policy.count=0; policy.preferred=255;
    dirty(); apply_bluetooth();
}
bool prefer(const bluetooth::Peer *peer) {
    int found=peer?storage::config.ble.find(*peer):-1;
    if(peer && found<0) return false;
    storage::config.ble.preferred=peer?uint8_t(found):255;
    dirty(); apply_bluetooth(false); return true;
}
void set_mode(bluetooth::Mode mode) {
    storage::config.ble.mode=mode; dirty(); apply_bluetooth();
}
bool command(const char *line) {
    if(!std::strcmp(line,"serial on") || !std::strcmp(line,"serial off") ||
       !std::strcmp(line,"ble on") || !std::strcmp(line,"ble off")) {
        bool serial=!std::strncmp(line,"serial ",7);
        bool enabled=!std::strcmp(line+(serial?7:4),"on");
        set_enabled(serial?input::Source::Serial:input::Source::Ble,enabled);
        if(!serial) apply_bluetooth();
        std::printf("%s: %s REQUESTED; use save to persist\n",serial?"SERIAL":"BLE",enabled?"ON":"OFF");
    } else if(!std::strcmp(line,"ble scan")) {
        set_enabled(input::Source::Ble,true);
        apply_bluetooth(); std::printf("BLE: SCAN REQUESTED\n");
    } else if(!std::strcmp(line,"ble target auto")) {
        set_mode(bluetooth::Mode::AutoCompatible);
        std::printf("BLE: TARGET auto; use save to persist\n");
    } else if(!std::strncmp(line,"ble target ",11)) {
        bluetooth::Peer key;
        if(!parse_peer(line+11,key)) std::printf("BLE: INVALID_TARGET; expected MAC public|random\n");
        else {
            storage::config.ble={}; whitelist_add(key,true); set_mode(bluetooth::Mode::WhitelistOnly);
            std::printf("BLE: TARGET CHANGED; use save to persist\n");
        }
    } else if(!std::strcmp(line,"pad status")) {
        auto serial=input::hub.status(input::Source::Serial),ble=input::hub.status(input::Source::Ble);
        std::printf("PAD: active=%s serial_enabled=%u serial_ready=%u ble_enabled=%u ble_ready=%u\n",input::name(input::hub.active_remote()),
            serial.enabled,serial.available,ble.enabled,ble.available);
    } else if(!std::strcmp(line,"ble discover")) {
        std::printf("BLE: DISCOVERY %s\n",ble::discover()?"REQUESTED":"BUSY OR DISABLED");
    } else if(!std::strcmp(line,"ble discover stop")) {
        ble::stop_discovery(); std::printf("BLE: AUTOMATIC CONNECTION RESUMED\n");
    } else if(!std::strcmp(line,"ble devices")) {
        ble::Candidate devices[16]; size_t count=ble::candidates(devices,16);
        for(size_t i=0;i<count;++i) {
            char mac[18]; devices[i].peer.text(mac);
            std::printf("BLE DEVICE: %s %s RSSI=%d HID=%u TGP=%u id=%u name=%s\n",mac,
                devices[i].peer.random?"random":"public",devices[i].rssi,devices[i].hid,devices[i].tgp_version,devices[i].controller_id,devices[i].name);
        }
        std::printf("BLE: DEVICES=%u\n",unsigned(count));
    } else if(!std::strcmp(line,"ble whitelist")) {
        const auto &policy=storage::config.ble;
        std::printf("BLE: mode=%s entries=%u\n",policy.mode==bluetooth::Mode::WhitelistOnly?"whitelist":"auto",policy.count);
        for(unsigned i=0;i<policy.count;++i) {
            char mac[18]; policy.peers[i].text(mac);
            std::printf("BLE ALLOWED: %s %s%s\n",mac,policy.peers[i].random?"random":"public",i==policy.preferred?" preferred":"");
        }
    } else if(!std::strcmp(line,"ble whitelist clear")) {
        whitelist_clear(); std::printf("BLE: WHITELIST CLEARED; use save to persist\n");
    } else if(!std::strcmp(line,"ble mode auto") || !std::strcmp(line,"ble mode whitelist")) {
        set_mode(!std::strcmp(line,"ble mode auto")?bluetooth::Mode::AutoCompatible:bluetooth::Mode::WhitelistOnly);
        std::printf("BLE: MODE CHANGED; use save to persist\n");
    } else {
        const char *value=nullptr; enum { Connect,Add,Remove,Prefer } operation=Connect;
        if(!std::strncmp(line,"ble connect ",12)) value=line+12;
        else if(!std::strncmp(line,"ble allow ",10)) {value=line+10; operation=Add;}
        else if(!std::strncmp(line,"ble forget ",11)) {value=line+11; operation=Remove;}
        else if(!std::strncmp(line,"ble prefer ",11)) {value=line+11; operation=Prefer;}
        else return false;
        bluetooth::Peer key; bool ok=parse_peer(value,key);
        if(ok) {
            if(operation==Connect) ok=ble::connect(key);
            else if(operation==Remove) ok=whitelist_remove(key);
            else ok=whitelist_add(key,operation==Prefer);
        }
        std::printf("BLE: %s%s\n",ok?"REQUEST ACCEPTED":"REQUEST REJECTED",
            ok && operation!=Connect?"; use save to persist":"");
    }
    return true;
}
}
