#include "input.hpp"
#include "menu_repeat.hpp"
#include "bluetooth_config.hpp"
#include <cassert>
#include <cstring>
using namespace gamepad;
using input::Source;
static void send(Receiver &receiver,uint16_t keys=0,uint8_t hat=255,uint32_t now=0) {
    uint8_t report[]={128,128,128,128,hat,uint8_t(keys),uint8_t(keys>>8),0,0,0};
    assert(receiver.receive(report,10,now));
}
static input::Snapshot take_menu(input::Hub &hub) {
    auto pad=hub.gamepad(); auto result=input::menu(pad); hub.acknowledge(pad); return result;
}
static bool take_home(input::Hub &hub) {
    auto pad=hub.gamepad(); bool result=pad.pressed&KeyHome; hub.acknowledge(pad); return result;
}
int main() {
    {
        input::Hub unified; Receiver remote,backup;
        assert(!unified.status(Source::Local).available);
        unified.local(0);
        assert(unified.status(Source::Local).available && unified.status(Source::Local).active);
        assert(unified.active_remote()==Source::None && unified.local_buttons()==0);
        unified.attach(Source::Serial,remote); unified.attach(Source::Ble,backup);
        send(remote); send(backup);
        unified.acknowledge(unified.gamepad());
        send(remote,0x08); send(remote); // A complete X tap between UI ticks.
        auto tap=unified.gamepad();
        assert(tap.pressed==KeyX && tap.released==KeyX && !tap.controller.keys);
        assert(unified.gamepad().pressed==KeyX);
        assert(!input::nes(tap) && !input::menu(tap).pressed);
        assert(unified.gamepad().released==KeyX); // Mappers never consume events.
        send(remote,0x10);
        unified.acknowledge(tap); // Stale acknowledgement cannot erase Y.
        assert(unified.gamepad().pressed==(KeyX|KeyY));
        unified.acknowledge(unified.gamepad());
        assert(!unified.gamepad().pressed);
        send(remote); unified.acknowledge(unified.gamepad());
        unified.local(input::LocalB); auto down=unified.gamepad();
        assert(down.controller.keys==KeyA && down.pressed==KeyA);
        assert(unified.status(Source::Local).controller.keys==KeyA);
        unified.acknowledge(down); send(remote,1);
        assert(!unified.gamepad().pressed); // Second contributor is not a new press.
        unified.local(0); assert(!unified.gamepad().released);
        send(remote); assert(unified.gamepad().released==KeyA);
        unified.acknowledge(unified.gamepad());

        ControllerState analog; analog.left_x=255; analog.right_y=0; analog.lt=97;
        remote.update(analog,20); unified.local(input::LocalUp);
        auto pad=unified.gamepad();
        assert(pad.controller.left_x==255 && pad.controller.right_y==0 && pad.controller.lt==97);
        assert(pad.controller.dpad==North && pad.directions==(North|East));
        analog.dpad=West; remote.update(analog,21);
        assert(unified.gamepad().directions==(North|West)); // Physical D-pad overrides remote stick only.
        unified.clear_events(); auto page=unified.gamepad();
        assert(page.controller.neutral() && !page.pressed && !page.directions);
        assert(unified.status(Source::Serial).controller.right_y==0);
        unified.local(0); analog={}; analog.keys=KeyRB; remote.update(analog,22);
        assert(!unified.neutral() && unified.gamepad().controller.neutral());
        analog.keys=0; analog.right_x=255; remote.update(analog,23);
        assert(!unified.neutral()); // Full controls, not just menu-visible keys.
        send(remote); send(remote,1);
        assert(unified.gamepad().pressed==KeyA);
        unified.acknowledge(page); assert(unified.gamepad().pressed==KeyA);
        send(remote); unified.acknowledge(unified.gamepad());
        // A short Home tap suppresses confirmation from the same dispatch.
        unified.local(input::LocalHome); unified.local(0); unified.local(input::LocalB);
        pad=unified.gamepad();
        assert(pad.pressed&KeyHome); assert(!input::menu(pad).pressed && !input::menu(pad).held);
        remote.release(); assert(unified.gamepad().pressed&KeyHome); // Local Home survives handover.
        unified.acknowledge(unified.gamepad()); unified.local(0); unified.acknowledge(unified.gamepad());
        send(backup); send(backup,0x08);
        Receiver replacement;
        unified.attach(Source::Ble,replacement); // Same port, different controller session.
        assert(!unified.gamepad().pressed && !unified.gamepad().released);
        send(replacement,1); assert(!unified.status(Source::Ble).armed);
        send(replacement); send(replacement,1); assert(unified.gamepad().pressed==KeyA);
        send(replacement); unified.acknowledge(unified.gamepad());
        unified.local(input::LocalHome); send(replacement); replacement.release();
        assert(unified.gamepad().pressed&KeyHome); // Heartbeats cannot erase pending Local Home.
    }
    input::Hub home_edges;
    home_edges.local(input::LocalHome);
    assert(home_edges.home() && take_home(home_edges));
    assert(!take_home(home_edges) && !take_menu(home_edges).pressed);
    home_edges.clear_events();
    home_edges.local(input::LocalHome|input::LocalB);
    assert(!take_home(home_edges) && !take_menu(home_edges).pressed);
    home_edges.local(0); assert(home_edges.neutral());
    home_edges.local(input::LocalHome); assert(take_home(home_edges));
    home_edges.clear_events(); assert(!take_home(home_edges));
    // Menu paging is independent of game selection; NES inputs remain intact.
    input::Hub navigation;
    const uint8_t mappings[][2]={{input::LocalA,input::Previous},{input::LocalC,input::Next},
        {input::LocalUp,input::PagePrevious},{input::LocalDown,input::PageNext}};
    for(const auto &pair:mappings) {
        navigation.local(pair[0]);
        assert(take_menu(navigation).pressed==pair[1]);
        navigation.local(0); take_menu(navigation);
    }
    Receiver remote;
    navigation.attach(Source::Ble,remote); send(remote);
    for(uint8_t hat=0;hat<8;hat+=2) {
        send(remote,0,hat);
        const uint8_t events[]={input::PagePrevious,input::Next,input::PageNext,input::Previous};
        assert(take_menu(navigation).pressed==events[hat/2]);
        assert(input::nes(navigation.gamepad())==(hat==0?Up:hat==2?Right:hat==4?Down:Left));
        send(remote); take_menu(navigation);
    }
    // X/Y repeat from elapsed time even when a HID controller only sends changes.
    send(remote,0x18,6); // X+Y while holding left.
    assert(take_menu(navigation).pressed==input::Previous);
    for(uint32_t now=0;now<992;++now) {
        const uint8_t pulse=(now/62)%2?0:A|B;
        assert(input::nes(navigation.gamepad(),false,now)==(Left|pulse));
        assert(!take_menu(navigation).pressed); // No generated confirm/back edges.
    }
    send(remote,0x08); // X alone.
    assert(input::nes(navigation.gamepad(),false,124)==A && input::nes(navigation.gamepad(),true,124)==B);
    assert(!input::nes(navigation.gamepad(),false,186));
    send(remote,0x10); // Y alone.
    assert(input::nes(navigation.gamepad(),false,248)==B && input::nes(navigation.gamepad(),true,248)==A);
    send(remote,0x1b); // Ordinary A/B take priority over turbo's off phase.
    assert(input::nes(navigation.gamepad(),false,186)==(A|B));
    send(remote,0x08,255,125); // Repeated reports do not restart the pulse phase.
    send(remote,0x08,255,186); assert(!input::nes(navigation.gamepad(),false,186));
    navigation.local(input::LocalB); assert(input::nes(navigation.gamepad(),false,186)==A);
    navigation.local(0); remote.release(); assert(!input::nes(navigation.gamepad(),false,248));
    send(remote,0x18); assert(!input::nes(navigation.gamepad(),false,248)); // Held reconnect is blocked.
    send(remote); send(remote,0x18); assert(input::nes(navigation.gamepad(),false,248)==(A|B));
    navigation.enable(Source::Ble,false); assert(!input::nes(navigation.gamepad(),false,248));
    navigation.enable(Source::Ble,true); send(remote); send(remote,0x18);
    remote.receive(nullptr,0); assert(!input::nes(navigation.gamepad(),false,248));
    send(remote); take_menu(navigation);
    send(remote,0x0800); assert(take_menu(navigation).pressed==input::Favorite && input::nes(navigation.gamepad())==Start);
    assert(!take_menu(navigation).pressed); send(remote); take_menu(navigation);
    send(remote,0x0400); assert(take_menu(navigation).pressed==input::List && input::nes(navigation.gamepad())==Select);
    navigation.clear_events(); assert(!take_menu(navigation).held);
    send(remote); take_menu(navigation);
    menu::Repeat repeat;
    input::Snapshot held{input::Next,input::Next,0};
    assert(repeat.poll(held,100)==input::Next);
    held.pressed=0;
    assert(!repeat.poll(held,499));
    assert(repeat.poll(held,500)==input::Next);
    assert(!repeat.poll(held,649));
    assert(repeat.poll(held,650)==input::Next);
    assert(repeat.poll(held,5000)==input::Next); // No backlog replay.
    assert(!repeat.poll(held,5000));
    ++held.epoch; assert(!repeat.poll(held,5100));
    assert(!repeat.poll(held,5499)); assert(repeat.poll(held,5500)==input::Next);
    held={}; assert(!repeat.poll(held,5600));
    held={input::Confirm,input::Confirm,0}; assert(repeat.poll(held,6000)==input::Confirm);
    held.pressed=0; assert(!repeat.poll(held,9000));
    repeat.reset(); held={input::PageNext,input::PageNext,0};
    assert(repeat.poll(held,0xffffff00u)==input::PageNext);
    held.pressed=0; assert(!repeat.poll(held,143)); assert(repeat.poll(held,144)==input::PageNext);
    const uint8_t axes[]={input::Previous|input::Next,input::PagePrevious|input::PageNext};
    for(const auto axis : axes) {
        repeat.reset(); held={axis,axis,0};
        assert(!repeat.poll(held,0));
        held.pressed=0; assert(!repeat.poll(held,1000));
        const uint8_t single=axis&uint8_t(-axis);
        held={single,single,0}; // Even a backend reissued edge must restart the delay.
        assert(!repeat.poll(held,1001));
        held.pressed=0; assert(!repeat.poll(held,1400));
        assert(repeat.poll(held,1401)==single);
        held={axis,axis,0}; assert(!repeat.poll(held,1551));
    }
    assert(menu::launch_requested(input::Confirm));
    assert(!menu::launch_requested(input::Confirm|input::Next));
    assert(!menu::launch_requested(input::Confirm|input::Previous));
    assert(!menu::launch_requested(input::Confirm|input::Favorite));
    assert(!menu::launch_requested(input::Confirm|input::List));
    repeat.reset();
    assert(!repeat.poll({input::Favorite|input::List,input::Favorite|input::List,0},0));
    assert(!repeat.poll({input::Favorite,input::Favorite,0},1));
    assert(!repeat.poll({input::Favorite,0,0},10000));
    repeat.poll({},10001);
    assert(repeat.poll({input::Favorite,input::Favorite,0},10002)==input::Favorite);
    assert(!repeat.poll({input::Favorite,0,0},20000));
    // A held direction behind the input barrier cannot trigger menu repeat.
    navigation.local(input::LocalC); navigation.clear_events();
    assert(!take_menu(navigation).held); navigation.local(0);

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
    send(ble,2,255,1); assert(hub.active_remote()==Source::Serial && !input::nes(hub.gamepad()));
    send(serial,1); send(serial); // Short press must survive until menu consumes it.
    auto events=take_menu(hub); assert(!events.held && events.pressed==input::Confirm && events.released==input::Confirm);
    assert(!take_menu(hub).pressed);
    send(serial,1); hub.local(input::LocalB); take_menu(hub);
    send(serial); assert(!take_menu(hub).released); // Local still holds the same menu action.
    hub.local(0); assert(take_menu(hub).released==input::Confirm);
    send(serial,1); hub.local(input::LocalA);
    serial.release(); assert(hub.active_remote()==Source::Ble);
    assert(input::nes(hub.gamepad())==Left && !hub.status(Source::Ble).armed); // Local left survives, held backup B is blocked.
    send(ble); assert(hub.status(Source::Ble).armed);
    hub.local(0); take_menu(hub); send(ble,2); assert(input::nes(hub.gamepad())==B);
    assert(input::nes(hub.gamepad(),true)==A);
    send(serial,1); assert(input::nes(hub.gamepad())==B); // Previously active serial cannot steal selection back.
    hub.enable(Source::Ble,false); assert(hub.active_remote()==Source::Serial && !input::nes(hub.gamepad()));
    send(serial); send(serial,1); assert(input::nes(hub.gamepad())==A);
    hub.enable(Source::Serial,false); assert(hub.active_remote()==Source::None && !input::nes(hub.gamepad()));
    hub.local(input::LocalHome); assert(hub.home()); hub.local(0);
    hub.enable(Source::Ble,true); send(ble); send(ble,0x6000,255,100);
    ble.poll(1600); assert(!hub.home()); // Stick clicks no longer trigger Home.
    send(ble); send(ble,0x00c0,255,1700);
    ble.poll(3199); assert(!hub.home()); ble.poll(3200); assert(hub.home());
    send(ble); hub.clear_events(); send(ble,1); send(ble);
    assert(take_menu(hub).pressed==input::Confirm);
    hub.clear_events(); send(ble,1); hub.clear_events();
    assert(!take_menu(hub).pressed); send(ble); send(ble,1); assert(take_menu(hub).pressed==input::Confirm);

    // A non-selected source cannot complete Home, even after a long idle interval.
    hub.enable(Source::Serial,true); send(serial,0x00c0,255,4000); serial.poll(10000); assert(!serial.home);
    send(ble,0x0040,255,4000); send(serial,0x0080,255,4000); ble.poll(10000); assert(!hub.home());
    // A healthy BLE source keeps its selection without notifications.
    assert(hub.active_remote()==Source::Ble);

    // A new adapter can publish normalized state without constructing XTARK bytes.
    ble.update({},11000); take_menu(hub);
    ControllerState adapted; adapted.keys=KeyA; adapted.dpad=West;
    ble.update(adapted,11001); assert(input::nes(hub.gamepad())==(A|Left));
    take_menu(hub); hub.local(input::LocalC); hub.local(0);
    ble.release(); // Source handover discards remote edges, preserving the local short tap.
    auto local_events=take_menu(hub); assert(local_events.pressed==input::Next);

    bluetooth::Policy policy; bluetooth::Peer peer;
    assert(bluetooth::address("AA:BB:CC:DD:EE:FF",peer.address));
    assert(policy.add(peer) && policy.add(peer) && policy.count==1);
    peer.random=true; assert(policy.add(peer) && policy.count==2); policy.preferred=1;
    auto first=policy.peers[0]; assert(policy.remove(first) && policy.preferred==0 && policy.count==1);
    assert(policy.remove(peer) && policy.preferred==255 && !policy.count);
    for(unsigned i=0;i<bluetooth::Policy::Capacity;++i) {peer.address[0]=i; assert(policy.add(peer));}
    peer.address[0]=99; assert(!policy.add(peer));
}
