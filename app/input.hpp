#pragma once
#include "gamepad.hpp"

// Platform-independent input arbitration. All calls run on core0.
namespace input {
enum class Source : uint8_t { None,Serial,Ble,Local };
enum MenuKey : uint8_t { Previous=1,Next=2,Confirm=4,Back=8,PagePrevious=16,PageNext=32,Favorite=64,List=128 };
enum LocalKey : uint8_t { LocalA=1,LocalB=2,LocalC=4,LocalUp=8,LocalDown=16,LocalHome=32 };
struct Snapshot {uint8_t held=0,pressed=0,released=0; uint32_t epoch=0;};
// Keys occupy bits 0..15; effective navigation directions occupy bits 16..19.
// ControllerState retains the physical D-pad and both analog sticks separately.
struct Gamepad {
    gamepad::ControllerState controller;
    uint8_t directions=0;
    uint32_t pressed=0,released=0,epoch=0,revision=0;
};
Snapshot menu(const Gamepad &pad);
uint8_t nes(const Gamepad &pad,bool swap_ab=false,uint32_t now_ms=0);
struct SourceStatus {
    // Local is eligible after its first sample; active is not exclusive.
    bool enabled=true,available=false,active=false,armed=false;
    gamepad::ControllerState controller;
};
class Hub {
public:
    void attach(Source source,gamepad::Receiver &receiver);
    void enable(Source source,bool enabled);
    SourceStatus status(Source source) const;
    Source active_remote() const {return active;}
    void local(uint8_t keys);
    uint8_t local_buttons() const {return local_keys;}
    Gamepad gamepad() const;
    // Dispatcher owns the copied events after acknowledgement. Stale snapshots
    // cannot clear events from a subsequent update or page change.
    void acknowledge(const Gamepad &pad);
    bool home() const {return merged.controller.keys&gamepad::KeyHome;}
    bool neutral() const {return merged.controller.neutral();}
    uint32_t epoch() const {return merged.epoch;}
    void clear_events();
private:
    struct Port {gamepad::Receiver *receiver=nullptr; bool enabled=true,armed=false;};
    Port ports[2];
    Source active=Source::None;
    uint8_t local_keys=0;
    Gamepad merged;
    uint32_t local_pressed=0,local_released=0;
    bool barrier=false,local_ready=false;
    static void changed(void *context,gamepad::Receiver &receiver);
    void update();
};
extern Hub hub;
const char *name(Source source);
}
