#pragma once
#include "gamepad.hpp"

// Platform-independent input arbitration. All calls run on core0.
namespace input {
enum class Source : uint8_t { None,Serial,Ble };
enum MenuKey : uint8_t { Previous=1,Next=2,Confirm=4,Back=8 };
enum LocalKey : uint8_t { LocalA=1,LocalB=2,LocalC=4,LocalUp=8,LocalDown=16,LocalHome=32 };
struct Snapshot {uint8_t held=0,pressed=0,released=0;};
struct SourceStatus {
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
    uint8_t nes(bool swap_ab=false) const;
    bool home() const;
    Snapshot take_menu();
    void clear_events();
private:
    struct Port {gamepad::Receiver *receiver=nullptr; bool enabled=true,armed=false;};
    Port ports[2];
    Source active=Source::None;
    uint8_t local_keys=0;
    Snapshot menu;
    uint8_t local_pressed=0,local_released=0;
    bool barrier=false;
    static void changed(void *context,gamepad::Receiver &receiver);
    void update();
    uint8_t remote_keys() const;
};
extern Hub hub;
const char *name(Source source);
}
