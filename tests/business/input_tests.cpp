#include "input.hpp"
#include "bluetooth_config.hpp"
#include <cassert>
#include <cstring>
using namespace gamepad;
using input::Source;
static void send(Receiver &receiver,uint16_t keys=0,uint8_t hat=255,uint32_t now=0) {
    uint8_t report[]={128,128,128,128,hat,uint8_t(keys),uint8_t(keys>>8),0,0,0};
    assert(receiver.receive(report,10,now));
}
int main() {
    ControllerState state;
    assert(state.neutral() && state.left_x==128 && state.right_y==128);
    uint8_t raw[]={1,2,3,4,3,0xdb,0x7f,65,129,0};
    assert(decode(raw,10,state));
    assert(state.keys==(KeyA|KeyB|KeyX|KeyY|KeyLB|KeyRB|KeyLT|KeyRT|KeySelect|KeyStart|KeyHome|KeyL3|KeyR3));
    assert(state.dpad==(East|South) && state.left_x==1 && state.left_y==2 && state.right_x==3 && state.right_y==4);
    assert(state.lt==129 && state.rt==65 && !state.neutral());

    Receiver serial,ble; input::Hub hub;
    hub.attach(Source::Serial,serial); hub.attach(Source::Ble,ble);
    send(serial); assert(hub.active_remote()==Source::Serial);
    send(ble,2,255,1); assert(hub.active_remote()==Source::Serial && !hub.nes());
    send(serial,1); send(serial); // Short press must survive until menu consumes it.
    auto events=hub.take_menu(); assert(!events.held && events.pressed==input::Confirm && events.released==input::Confirm);
    assert(!hub.take_menu().pressed);
    send(serial,1); hub.local(input::LocalB); hub.take_menu();
    send(serial); assert(!hub.take_menu().released); // Local still holds the same menu action.
    hub.local(0); assert(hub.take_menu().released==input::Confirm);
    send(serial,1); hub.local(input::LocalA);
    serial.release(); assert(hub.active_remote()==Source::Ble);
    assert(hub.nes()==Left && !hub.status(Source::Ble).armed); // Local left survives, held backup B is blocked.
    send(ble); assert(hub.status(Source::Ble).armed);
    hub.local(0); hub.take_menu(); send(ble,2); assert(hub.nes()==B);
    assert(hub.nes(true)==A);
    send(serial,1); assert(hub.nes()==B); // Previously active serial cannot steal selection back.
    hub.enable(Source::Ble,false); assert(hub.active_remote()==Source::Serial && !hub.nes());
    send(serial); send(serial,1); assert(hub.nes()==A);
    hub.enable(Source::Serial,false); assert(hub.active_remote()==Source::None && !hub.nes());
    hub.local(input::LocalHome); assert(hub.home()); hub.local(0);
    hub.enable(Source::Ble,true); send(ble); send(ble,0x6000,255,100);
    ble.poll(1599); assert(!hub.home()); ble.poll(1600); assert(hub.home());
    send(ble); hub.clear_events(); send(ble,1); send(ble);
    assert(hub.take_menu().pressed==input::Confirm);
    hub.clear_events(); send(ble,1); hub.clear_events();
    assert(!hub.take_menu().pressed); send(ble); send(ble,1); assert(hub.take_menu().pressed==input::Confirm);

    // A non-selected source cannot complete Home, even after a long idle interval.
    hub.enable(Source::Serial,true); send(serial,0x6000,255,2000); serial.poll(10000); assert(!serial.home);
    send(ble,0x2000,255,2000); send(serial,0x4000,255,2000); ble.poll(10000); assert(!hub.home());
    // A healthy BLE source keeps its selection without notifications.
    assert(hub.active_remote()==Source::Ble);

    // A new adapter can publish normalized state without constructing XTARK bytes.
    ble.update({},11000); hub.take_menu();
    ControllerState adapted; adapted.keys=KeyA; adapted.dpad=West;
    ble.update(adapted,11001); assert(hub.nes()==(A|Left));
    hub.take_menu(); hub.local(input::LocalC); hub.local(0);
    ble.release(); // Source handover discards remote edges, preserving the local short tap.
    auto local_events=hub.take_menu(); assert(local_events.pressed==input::Next);

    bluetooth::Policy policy; bluetooth::Peer peer;
    assert(bluetooth::address("AA:BB:CC:DD:EE:FF",peer.address));
    assert(policy.add(peer) && policy.add(peer) && policy.count==1);
    peer.random=true; assert(policy.add(peer) && policy.count==2); policy.preferred=1;
    auto first=policy.peers[0]; assert(policy.remove(first) && policy.preferred==0 && policy.count==1);
    assert(policy.remove(peer) && policy.preferred==255 && !policy.count);
    for(unsigned i=0;i<bluetooth::Policy::Capacity;++i) {peer.address[0]=i; assert(policy.add(peer));}
    peer.address[0]=99; assert(!policy.add(peer));
}
