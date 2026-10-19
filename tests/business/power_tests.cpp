#include "power_policy.hpp"
#include <cassert>
#include <cstring>
#include <string>

using namespace power;
static std::string events;
static unsigned checks=0, block_check=0;
static bool saved=true;
static uint32_t entered=0;
static bool ready() {events+='R'; return ++checks!=block_check;}
static bool save() {events+='S'; return saved;}
static Error enter(uint32_t duration) {events+='E'; entered=duration; return Error::None;}
static const Hooks hooks{ready,save,enter};
static void reset() {events.clear(); checks=block_check=0; saved=true; entered=0;}

int main() {
    IdleTimer timer;
    assert(!timer.due(1000000,300));
    timer.menu(true,100);
    assert(!timer.due(300099,300)); assert(timer.due(300100,300));
    timer.activity(300100); assert(!timer.due(300101,300));
    timer.menu(false,300101); assert(!timer.due(100000000,300));
    timer.menu(true,100000000); assert(!timer.due(100000001,300));
    assert(timer.due(100300000,300));
    assert(!timer.due(100300000,0)); // Disable auto sleep.
    timer.activity(0xfffffff0u);
    assert(!timer.due(983,1)); assert(timer.due(984,1)); // Millisecond wrap.
    timer.menu(false,985); assert(!timer.due(1000000,1)); // Disk/game never sleep.
    timer.menu(true,1000000);
    for(uint32_t now=1000000;now<2000000;now+=10) {
        timer.activity(now); assert(!timer.due(now,1)); // Held input.
    }
    assert(timer.due(2000990,1));

    uint32_t value=123;
    assert(seconds("0",kMaxIdleSeconds,value) && value==0);
    assert(seconds("86400",kMaxIdleSeconds,value) && value==86400);
    assert(seconds("4294967",kMaxSleepSeconds,value) && value*1000u==4294967000u);
    for(auto text:{"", "-1", "+1", " 1", "1 ", "1.5", "86401", "4294967296", "99999999999999999999"}) {
        value=123; assert(!seconds(text,kMaxIdleSeconds,value)); assert(value==123);
    }
    assert(!seconds("4294968",kMaxSleepSeconds,value));

    Request request;
    reset(); assert(request.queue(5000,hooks));
    assert(events=="R" && request.pending()); // No save or teardown in callback.
    assert(!request.queue(0,hooks)); assert(request.duration()==5000);
    request.run(hooks);
    assert(events=="RRSRE" && entered==5000 && !request.pending());
    assert(request.error()==Error::Backend); // Unexpected backend return.
    request.run(hooks); assert(events=="RRSRE");

    reset(); block_check=1;
    assert(!request.queue(0,hooks)); assert(!request.pending());
    assert(events=="R" && request.error()==Error::Busy);
    reset(); block_check=2; assert(request.queue(0,hooks));
    request.run(hooks); assert(events=="RR" && request.error()==Error::Busy);
    reset(); saved=false; assert(request.queue(0,hooks));
    request.run(hooks); assert(events=="RRS" && request.error()==Error::Unsaved);
    reset(); block_check=3; assert(request.queue(0,hooks));
    request.run(hooks); assert(events=="RRSR" && request.error()==Error::Busy);
    // Failure can be retried; no old request is replayed implicitly.
    reset(); assert(request.queue(1000,hooks)); request.run(hooks);
    assert(entered==1000 && events=="RRSRE");
}
