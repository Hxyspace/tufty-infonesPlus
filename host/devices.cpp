#include "runtime.hpp"
#include "battery.hpp"
#include "ble.hpp"
#include "usb.hpp"
#include "power.hpp"
#include "display.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>

namespace preview {
namespace {
input::Source keyboard_source=input::Source::Ble;
battery::Reading battery_value{0,0,3800,72,0,true};
}
input::Source source() {return keyboard_source;}
void source(input::Source value) {
    usb::pad.input.release(); ble::input_receiver().release();
    keyboard_source=value;
    std::printf("HOST controller source: %s\n",input::name(value));
}
void battery_percent(unsigned value) {battery_value.percent=uint8_t(value); battery_value.timestamp_ms=micros()/1000;}
void devices_init() {ble::init();}
void devices_poll(const gamepad::ControllerState &keys) {
    ble::poll();
    const auto now=uint32_t(micros()/1000);
    auto &serial=usb::pad.input;
    auto &bluetooth=ble::input_receiver();
    if(keyboard_source==input::Source::Serial && input::hub.status(input::Source::Serial).enabled)
        serial.update(keys,now);
    else if(serial.valid) serial.release();
    if(keyboard_source==input::Source::Ble && ble::status().state==ble::State::Ready)
        bluetooth.update(keys,now);
    else if(bluetooth.valid) bluetooth.release();
    serial.poll(now); bluetooth.poll(now);
}
}
uint64_t get_absolute_time() {return preview::micros();}
void tufty_service() {preview::service();}

namespace usb {
tgp::Receiver pad;
bool dirty=false,save_requested=false,disk_requested=false,rescan_requested=false,boot_requested=false;
}
namespace battery {
void init() {}
bool refresh(uint32_t now) {preview::battery_value.timestamp_ms=now; return true;}
const Reading &reading() {return preview::battery_value;}
}
namespace power {void activity() {}}
namespace display {
namespace {bool held=false,te=true; uint8_t light=80;}
void pause(bool value) {held=value;}
bool paused() {return held;}
void brightness(uint8_t value) {light=value;}
void te_sync(bool value) {te=value;}
bool te_requested() {return te;}
Stats stats() {Stats s; s.te_enabled=te; return s;}
}

// The real selection policy is reused; the radio and its candidates are fake.
// No Bluetooth/USB discovery or device connection is performed on the PC.
namespace ble {
namespace {
bluetooth::Manager manager;
gamepad::Receiver receiver;
State state=State::Off;
bluetooth::Peer peer;
uint32_t scan_at=0;
bool scanning=false;
std::array<Candidate,3> demo() {
    std::array<Candidate,3> items{};
    for(unsigned i=0;i<items.size();++i) {
        items[i].peer.address[0]=0xc0; items[i].peer.address[5]=uint8_t(i+1);
        std::snprintf(items[i].name,sizeof(items[i].name),"Preview gamepad %u",i+1);
        items[i].hid=true; items[i].rssi=int8_t(-35-i*10);
    }
    return items;
}
}
gamepad::Receiver &input_receiver() {return receiver;}
Status status() {return {state,manager.configuration().enabled,false,manager.discovering(),peer,0};}
void configure(const bluetooth::Configuration &config,bool reconnect) {
    const bool changed=manager.configuration().enabled!=config.enabled;
    manager.configure(config,reconnect || changed);
}
bool discover(uint32_t duration) {return manager.discover(duration);}
void stop_discovery() {manager.stop_discovery();}
size_t candidates(Candidate *out,size_t size) {return manager.candidates(out,size);}
size_t candidate_count() {return manager.count();}
bool connect(const bluetooth::Peer &key) {return manager.connect(key);}
void init() {manager.configure(manager.configuration(),true);}
void poll() {
    const auto now=uint32_t(preview::micros()/1000);
    if(manager.take_request()) {
        receiver.release(); peer={};
        scanning=manager.configuration().enabled && manager.begin_scan(now);
        scan_at=now; state=scanning?State::Scanning:State::Off;
    }
    if(!scanning) return;
    if(now-scan_at>=300) {
        for(auto item:demo()) {
            item.seen_at=now; manager.observe(item,true);
            if(preview::source()==input::Source::Ble && manager.accepts(item.peer,true,now)) {
                peer=item.peer; state=State::Ready; scanning=false; return;
            }
        }
    }
    if(manager.finish_discovery(now)) {scanning=false; state=State::Waiting;}
}
void shutdown() {receiver.release(); manager.reset(); scanning=false; state=State::Off;}
}
namespace infones::port {
void *allocate(size_t bytes) {return std::malloc(bytes);}
void release(void *data) {std::free(data);}
bool has_large_memory() {return true;}
bool remove_sprite_limit() {return false;}
}
