#include "input.hpp"

namespace input {
Hub hub;
namespace {
int index(Source s) {return s==Source::Serial?0:s==Source::Ble?1:-1;}
gamepad::ControllerState local_state(uint8_t keys) {
    gamepad::ControllerState state;
    if(keys&LocalA) state.dpad|=gamepad::West;
    if(keys&LocalC) state.dpad|=gamepad::East;
    if(keys&LocalUp) state.dpad|=gamepad::North;
    if(keys&LocalDown) state.dpad|=gamepad::South;
    if(keys&LocalB) state.keys|=gamepad::KeyA;
    if(keys&LocalHome) state.keys|=gamepad::KeyHome;
    return state;
}
uint32_t bits(const Gamepad &pad) {return pad.controller.keys|(uint32_t(pad.directions)<<16);}
uint8_t menu_bits(uint32_t keys) {
    using namespace gamepad;
    return ((keys&(uint32_t(West)<<16))?Previous:0)|
        ((keys&(uint32_t(East)<<16))?Next:0)|
        ((keys&(uint32_t(North)<<16))?PagePrevious:0)|
        ((keys&(uint32_t(South)<<16))?PageNext:0)|
        ((keys&KeyA)?Confirm:0)|((keys&KeyB)?Back:0)|
        ((keys&KeyStart)?Favorite:0)|((keys&KeySelect)?List:0);
}
}
Snapshot menu(const Gamepad &pad) {
    if((pad.controller.keys|pad.pressed)&gamepad::KeyHome) return {0,0,0,pad.epoch};
    return {menu_bits(bits(pad)),menu_bits(pad.pressed),menu_bits(pad.released),pad.epoch};
}
uint8_t nes(const Gamepad &pad,bool swap_ab,uint32_t now_ms) {
    using namespace gamepad;
    const auto keys=pad.controller.keys;
    if(keys&KeyHome) return 0;
    const bool pulse=(now_ms/62)%2==0; // ble_gamepad: 62 ms on / 62 ms off.
    uint8_t result=((keys&KeyA || (pulse && (keys&KeyX)))?A:0)|
        ((keys&KeyB || (pulse && (keys&KeyY)))?B:0)|
        ((keys&KeySelect)?Select:0)|((keys&KeyStart)?Start:0)|
        ((pad.directions&North)?Up:0)|((pad.directions&South)?Down:0)|
        ((pad.directions&West)?Left:0)|((pad.directions&East)?Right:0);
    return swap_ab?uint8_t((result&~3)|((result&1)<<1)|((result&2)>>1)):result;
}
const char *name(Source s) {return s==Source::Serial?"serial":s==Source::Ble?"ble":s==Source::Local?"local":"none";}
void Hub::attach(Source source,gamepad::Receiver &receiver) {
    int i=index(source); if(i<0) return;
    if(ports[i].receiver) {
        ports[i].receiver->select(false);
        ports[i].receiver->changed=nullptr; ports[i].receiver->context=nullptr;
    }
    if(active==source) {
        active=Source::None; ++merged.epoch;
        merged.pressed=local_pressed; merged.released=local_released;
        merged.controller=local_state(local_keys); merged.directions=merged.controller.dpad;
    }
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
    if(source==Source::Local) return {local_ready,local_ready,local_ready,local_ready,local_state(local_keys)};
    int i=index(source); if(i<0) return {false,false,false,false,{}};
    const auto &p=ports[i];
    return {p.enabled,p.enabled && p.receiver && p.receiver->valid,source==active,p.armed,
        p.receiver?p.receiver->state:gamepad::ControllerState{}};
}
void Hub::update() {
    auto available=[&](int i) {return ports[i].enabled && ports[i].receiver && ports[i].receiver->valid;};
    const auto local=local_state(local_keys);
    uint32_t prior=bits(merged);
    int current=index(active);
    if(current>=0 && !available(current)) {
        ports[current].receiver->select(false); ports[current].armed=false;
        active=Source::None; current=-1; ++merged.epoch;
        // Keep Local edges; losing a remote is not a physical key release.
        merged.pressed=local_pressed; merged.released=local_released;
        prior=local.keys|(uint32_t(local.dpad)<<16);
    }
    if(current<0) {
        int chosen=-1;
        if(available(0)) chosen=0;
        if(available(1) && (chosen<0 || int32_t(ports[1].receiver->at-ports[0].receiver->at)>=0)) chosen=1;
        if(chosen>=0) {
            active=chosen==0?Source::Serial:Source::Ble; ++merged.epoch;
            ports[chosen].armed=false; ports[chosen].receiver->select(false);
            current=chosen;
        }
    }
    if(current>=0 && !ports[current].armed && ports[current].receiver->state.neutral()) {
        ports[current].armed=true; ports[current].receiver->select(true);
    }
    merged.controller={}; merged.directions=0;
    if(current>=0 && ports[current].armed) {
        const auto &remote=*ports[current].receiver;
        merged.controller=remote.state; merged.directions=remote.directions;
        if(remote.home) merged.controller.keys|=gamepad::KeyHome;
    }
    merged.controller.keys|=local.keys;
    merged.controller.dpad|=local.dpad;
    merged.directions|=local.dpad;
    const uint32_t held=bits(merged),pressed=held&~prior,released=prior&~held;
    ++merged.revision;
    // Home survives the application barrier and suppresses all menu actions,
    // including when a short Home press ends before the next UI dispatch.
    if(held&gamepad::KeyHome) {
        if(!barrier) ++merged.epoch;
        barrier=true;
        merged.pressed&=gamepad::KeyHome; merged.released&=gamepad::KeyHome;
        local_pressed&=gamepad::KeyHome; local_released&=gamepad::KeyHome;
    }
    merged.pressed|=pressed&gamepad::KeyHome;
    merged.released|=released&gamepad::KeyHome;
    if(barrier) {
        if(neutral()) barrier=false;
        return;
    }
    merged.pressed|=pressed; merged.released|=released;
}
void Hub::local(uint8_t keys) {
    local_ready=true;
    if(keys==local_keys) return;
    const uint32_t prior=bits(merged);
    const bool suppressed=barrier;
    local_keys=keys; update();
    local_pressed|=(bits(merged)&~prior)&gamepad::KeyHome;
    local_released|=(prior&~bits(merged))&gamepad::KeyHome;
    if(!suppressed && !barrier) {
        local_pressed|=bits(merged)&~prior; local_released|=prior&~bits(merged);
    }
}
Gamepad Hub::gamepad() const {
    auto result=merged;
    if(barrier) {
        result.controller={}; result.controller.keys=merged.controller.keys&gamepad::KeyHome;
        result.directions=0;
    }
    return result;
}
void Hub::acknowledge(const Gamepad &pad) {
    if(pad.revision!=merged.revision || pad.epoch!=merged.epoch) return;
    merged.pressed=merged.released=local_pressed=local_released=0;
}
void Hub::clear_events() {
    ++merged.epoch;
    merged.pressed=merged.released=local_pressed=local_released=0;
    barrier=true; update();
    merged.pressed=merged.released=0;
}
}
