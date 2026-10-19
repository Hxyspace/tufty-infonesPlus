#include "gamepad.hpp"
#include <cstring>

namespace gamepad {
namespace {
int8_t axis(uint8_t value,int8_t previous) {
    // Engage 48 counts from center; release 32 counts from center. Hysteresis
    // prevents slight movement at the threshold from repeatedly toggling keys.
    if(value<=80) return -1;
    if(value>=176) return 1;
    if(previous<0 && value<96) return -1;
    if(previous>0 && value>160) return 1;
    return 0;
}
bool centered(uint8_t value) {return value>=96 && value<=160;}
}
bool disconnect_report(const uint8_t *data,size_t bytes) {
    return data && bytes==ReportBytes && !std::memcmp(data,DisconnectReport,ReportBytes);
}
bool ControllerState::neutral() const {
    return !keys && !dpad && !lt && !rt && centered(left_x) && centered(left_y) &&
        centered(right_x) && centered(right_y);
}
bool decode(const uint8_t *data,size_t bytes,ControllerState &state) {
    if(!data || bytes!=ReportBytes || disconnect_report(data,bytes)) return false;
    // Hat is a four-bit absolute field with Null State; upper bits are padding.
    // Values outside its logical range 0..7 indicate no direction.
    uint8_t hat=data[4]&0x0f;
    constexpr uint8_t hats[]={North,North|East,East,East|South,South,South|West,West,West|North};
    state={};
    state.left_x=data[0]; state.left_y=data[1]; state.right_x=data[2]; state.right_y=data[3];
    state.dpad=hat<8?hats[hat]:0; state.rt=data[7]; state.lt=data[8];
    constexpr uint16_t byte5[]={KeyA,KeyB,0,KeyX,KeyY,0,KeyLB,KeyRB};
    constexpr uint16_t byte6[]={KeyLT,KeyRT,KeySelect,KeyStart,KeyHome,KeyL3,KeyR3,0};
    for(unsigned bit=0;bit<8;++bit) {
        if(data[5]&(1u<<bit)) state.keys|=byte5[bit];
        if(data[6]&(1u<<bit)) state.keys|=byte6[bit];
    }
    return true;
}
bool Receiver::receive(const uint8_t *data,size_t bytes,uint32_t now) {
    ControllerState decoded;
    if(!decode(data,bytes,decoded)) {release(); ++rejected; return false;}
    update(decoded,now); return true;
}
void Receiver::update(const ControllerState &value,uint32_t now) {
    state=value;
    valid=true; at=now;
    stick_x=axis(state.left_x,stick_x); stick_y=axis(state.left_y,stick_y);
    directions=state.dpad;
    if(!state.dpad) { // A pressed D-pad overrides the whole stick direction.
        if(stick_x<0) directions|=West;
        if(stick_x>0) directions|=East;
        if(stick_y<0) directions|=North;
        if(stick_y>0) directions|=South;
    }
    if(!(state.keys&(KeyL3|KeyR3))) combo_latched=false; // Release both clicks to re-arm.
    bool candidate=selected && state.keys==(KeyL3|KeyR3) && !state.dpad &&
        centered(state.left_x) && centered(state.left_y) && centered(state.right_x) && centered(state.right_y) &&
        !state.lt && !state.rt;
    if(candidate && !combo_holding) combo_at=now;
    combo_holding=candidate;
    home=(state.keys&KeyHome)!=0 || combo_latched;
    ++received; notify();
}
bool Receiver::poll(uint32_t now) {
    if(!combo_holding || combo_latched || uint32_t(now-combo_at)<1500) return false;
    combo_latched=true; home=true; notify();
    return true;
}
void Receiver::release() {
    state={}; directions=0; home=false; valid=false;
    stick_x=stick_y=0;
    combo_holding=combo_latched=false; combo_at=0;
    notify();
}
void Receiver::select(bool enabled) {
    selected=enabled; combo_holding=combo_latched=false; combo_at=0;
    home=(state.keys&KeyHome)!=0;
}
uint8_t Receiver::nes() const {
    if(!valid || combo_latched) return 0;
    return ((state.keys&KeyA)?A:0)|((state.keys&KeyB)?B:0)|
        ((state.keys&KeySelect)?Select:0)|((state.keys&KeyStart)?Start:0)|
        ((directions&North)?Up:0)|((directions&South)?Down:0)|
        ((directions&West)?Left:0)|((directions&East)?Right:0);
}
}
