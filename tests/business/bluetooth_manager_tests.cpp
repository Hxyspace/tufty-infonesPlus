#include "bluetooth_manager.hpp"
#include <cassert>
#include <cstring>

int main() {
    bluetooth::Manager manager;
    bluetooth::Candidate a,b,list[16];
    a.peer.address[0]=1; b.peer=a.peer; b.peer.random=true;
    a.hid=true; a.rssi=-60; a.seen_at=100;
    std::strcpy(a.name,"XTARK");
    assert(manager.begin_scan(100));
    assert(manager.observe(a,true));
    auto response=a; response.hid=false; response.rssi=-50; response.seen_at=200;
    std::strcpy(response.name,"XT");
    assert(manager.observe(response,false)->hid); // Scan response preserves HID and full name.
    manager.observe(b,false);
    assert(manager.candidates(list,16)==2); // Same MAC, different address type.
    assert(list[0].hid && list[0].rssi==-50 && list[0].seen_at==200);
    assert(!std::strcmp(list[0].name,"XTARK"));
    // Switching an Android peer between HID and TGP must replace cached profile
    // metadata; a name-only scan response must preserve the new profile.
    auto custom=a; custom.hid=false; custom.tgp=true; custom.controller_id=1; custom.tgp_version=2;
    const auto *cached=manager.observe(custom,true);
    assert(cached->tgp && !cached->hid && cached->controller_id==1);
    response.tgp=false; response.controller_id=0; response.tgp_version=0;
    cached=manager.observe(response,false);
    assert(cached->tgp && cached->controller_id==1);
    cached=manager.observe(a,true); assert(cached->hid && !cached->tgp && !cached->controller_id);
    for(unsigned i=2;i<30;++i) {b.peer.address[0]=i; manager.observe(b,false);}
    assert(manager.candidates(list,16)==16 && !manager.candidates(nullptr,16));

    // Runtime policy is a snapshot, independent of mutable persistent settings.
    bluetooth::Configuration config;
    config.policy.mode=bluetooth::Mode::WhitelistOnly;
    assert(config.policy.add(a.peer) && config.policy.add(b.peer));
    config.policy.preferred=0;
    manager.configure(config);
    config.policy.peers[0]=b.peer;
    assert(manager.take_request() && !manager.take_request());
    assert(manager.begin_scan(1000));
    assert(manager.accepts(a.peer,false,1000)); // Known devices need not advertise UUID.
    assert(!manager.accepts(b.peer,true,2999));
    assert(manager.accepts(b.peer,false,3000));
    auto unknown=a.peer; unknown.address[0]=99;
    assert(!manager.accepts(unknown,true,5000));

    // UI discovery pauses auto-connect and waits for selection after expiry.
    assert(!manager.discover(999) && manager.discover(1000));
    assert(manager.take_request() && manager.count()==0);
    assert(manager.begin_scan(6000));
    assert(!manager.accepts(a.peer,true,6000));
    assert(!manager.finish_discovery(6999) && manager.finish_discovery(7000));
    assert(!manager.discovering() && !manager.begin_scan(7000));
    assert(!manager.accepts(a.peer,true,7000));

    // Selection survives the old link's asynchronous disconnection.
    assert(manager.connect(unknown) && manager.take_request());
    manager.retry(); assert(manager.begin_scan(8000));
    assert(manager.accepts(unknown,false,8000) && !manager.accepts(a.peer,true,8000));
    manager.retry(); assert(manager.begin_scan(9000));
    assert(!manager.accepts(unknown,true,9000) && manager.accepts(a.peer,false,9000));

    // Changing preference must not tear down a healthy connection.
    config=manager.configuration(); config.policy.preferred=1;
    manager.configure(config,false); assert(!manager.take_request());
    config.enabled=false; manager.configure(config);
    assert(!manager.discover(1000) && !manager.connect(a.peer));
    assert(!manager.accepts(a.peer,true,10000));
    manager.reset(); assert(!manager.configuration().enabled && !manager.take_request());
}
