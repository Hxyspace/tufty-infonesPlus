#include "input.hpp"

namespace input {
Hub hub;
namespace {
int index(Source s) {return s==Source::Serial?0:s==Source::Ble?1:-1;}
uint8_t menu_map(uint8_t nes) {
    return ((nes&(gamepad::Left|gamepad::Up))?Previous:0)|
        ((nes&(gamepad::Right|gamepad::Down))?Next:0)|
        ((nes&gamepad::A)?Confirm:0)|((nes&gamepad::B)?Back:0);
}
}
const char *name(Source s) {return s==Source::Serial?"serial":s==Source::Ble?"ble":"none";}
void Hub::attach(Source source,gamepad::Receiver &receiver) {
    int i=index(source); if(i<0) return;
    if(ports[i].receiver) {ports[i].receiver->changed=nullptr; ports[i].receiver->context=nullptr;}
    ports[i].receiver=&receiver; ports[i].armed=false;
    receiver.context=this; receiver.changed=changed; receiver.select(false); update();
}
void Hub::changed(void *context,gamepad::Receiver &) {static_cast<Hub *>(context)->update();}
void Hub::enable(Source source,bool enabled) {
    int i=index(source); if(i<0 || ports[i].enabled==enabled) return;
    ports[i].enabled=enabled;
    if(ports[i].receiver) ports[i].receiver->release();
    update();
}
SourceStatus Hub::status(Source source) const {
    int i=index(source); if(i<0) return {false,false,false,false,{}};
    const auto &p=ports[i];
    return {p.enabled,p.enabled && p.receiver && p.receiver->valid,source==active,p.armed,
        p.receiver?p.receiver->state:gamepad::ControllerState{}};
}
void Hub::update() {
    auto available=[&](int i) {return ports[i].enabled && ports[i].receiver && ports[i].receiver->valid;};
    int current=index(active);
    if(current>=0 && !available(current)) {
        ports[current].receiver->select(false); ports[current].armed=false;
        active=Source::None; current=-1;
        // Discard accumulated remote edges at a handover, keeping held local keys.
        menu.pressed=local_pressed; menu.released=local_released;
    }
    if(current<0) {
        int chosen=-1;
        if(available(0)) chosen=0;
        if(available(1) && (chosen<0 || int32_t(ports[1].receiver->at-ports[0].receiver->at)>=0)) chosen=1;
        if(chosen>=0) {
            active=chosen==0?Source::Serial:Source::Ble;
            ports[chosen].armed=false; ports[chosen].receiver->select(false);
            current=chosen;
        }
    }
    if(current>=0 && !ports[current].armed && ports[current].receiver->state.neutral()) {
        ports[current].armed=true; ports[current].receiver->select(true);
    }
    uint8_t keys=menu_map(remote_keys());
    if(local_keys&(LocalA|LocalUp)) keys|=Previous;
    if(local_keys&(LocalC|LocalDown)) keys|=Next;
    if(local_keys&LocalB) keys|=Confirm;
    uint8_t prior=menu.held;
    menu.held=keys;
    if(home()) {barrier=true; menu.pressed=menu.released=local_pressed=local_released=0;}
    if(barrier) {
        if(!keys && !home()) barrier=false;
        return;
    }
    menu.pressed|=keys&~prior; menu.released|=prior&~keys;
}
uint8_t Hub::remote_keys() const {
    int i=index(active);
    return i>=0 && ports[i].armed?ports[i].receiver->nes():0;
}
bool Hub::home() const {
    int i=index(active);
    return (local_keys&LocalHome) || (i>=0 && ports[i].armed && ports[i].receiver->home);
}
void Hub::local(uint8_t keys) {
    if(keys==local_keys) return;
    uint8_t prior=menu.held;
    bool suppressed=barrier;
    local_keys=keys; update();
    if(!suppressed && !barrier) {
        local_pressed|=menu.held&~prior; local_released|=prior&~menu.held;
    }
}
uint8_t Hub::nes(bool swap_ab) const {
    uint8_t keys=remote_keys()|
        ((local_keys&LocalA)?gamepad::Left:0)|((local_keys&LocalB)?gamepad::A:0)|
        ((local_keys&LocalC)?gamepad::Right:0)|((local_keys&LocalUp)?gamepad::Up:0)|
        ((local_keys&LocalDown)?gamepad::Down:0);
    return swap_ab?uint8_t((keys&~3)|((keys&1)<<1)|((keys&2)>>1)):keys;
}
Snapshot Hub::take_menu() {
    Snapshot result=menu; menu.pressed=menu.released=local_pressed=local_released=0; return result;
}
void Hub::clear_events() {menu.pressed=menu.released=local_pressed=local_released=0; barrier=true; update();}
}
