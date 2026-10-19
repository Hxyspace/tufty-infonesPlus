#pragma once
#include <cstddef>
#include <cstdint>

namespace gamepad {
// NES output bits belong to the application mapping, never the wire format.
enum Button : uint8_t { A=1, B=2, Select=4, Start=8, Up=16, Down=32, Left=64, Right=128 };
enum Key : uint16_t {
    KeyA=1,KeyB=2,KeyX=4,KeyY=8,KeyLB=16,KeyRB=32,KeyLT=64,KeyRT=128,
    KeySelect=256,KeyStart=512,KeyL3=1024,KeyR3=2048,KeyHome=4096
};
enum Direction : uint8_t { North=1,South=2,West=4,East=8 };
struct ControllerState {
    uint16_t keys=0;
    uint8_t dpad=0,left_x=128,left_y=128,right_x=128,right_y=128,lt=0,rt=0;
    bool neutral() const;
};
// Shared XTARK input payload for BLE and CDC1, without a Report ID prefix.
constexpr size_t ReportBytes=10;
// End the originating controller session; never interpret this as button state.
constexpr uint8_t DisconnectReport[ReportBytes]={0x54,0x47,0x50,0x01,0x42,0x59,0x45,0,0,0xa5};
bool disconnect_report(const uint8_t *data,size_t bytes);
bool decode(const uint8_t *data,size_t bytes,ControllerState &state);
struct Receiver {
    ControllerState state;
    uint8_t directions=0;
    bool home=false;
    bool valid=false;
    uint32_t at=0;
    uint32_t received=0,rejected=0;
    using Observer=void (*)(void *,Receiver &);
    Observer changed=nullptr;
    void *context=nullptr;
    bool receive(const uint8_t *data,size_t bytes,uint32_t now=0);
    void update(const ControllerState &value,uint32_t now); // For future report adapters.
    bool poll(uint32_t now); // Returns true once when the menu combo fires.
    void release();
    void select(bool enabled); // Arbitration resets gestures on every handover.
    bool contributing() const {return selected;}
    uint8_t nes() const;
private:
    int8_t stick_x=0,stick_y=0;
    bool selected=true,combo_holding=false,combo_latched=false;
    uint32_t combo_at=0;
    void notify() {if(changed) changed(context,*this);}
};
}
