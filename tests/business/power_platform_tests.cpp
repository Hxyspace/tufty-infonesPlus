// Exercise the production command/transition adapter with board/USB mocks.
#include "power.hpp"
#include "power_board.hpp"
#include "storage.hpp"
#include "usb.hpp"
#include "pico/stdlib.h"
#include <cassert>
#include <string>

static uint32_t clock_ms=0;
static bool wake_idle=true, disk=false, save_ok=true, press_during_drain=false;
static unsigned saves=0;
static std::string events;
struct Sleeping {uint32_t duration;};
absolute_time_t get_absolute_time() {return uint64_t(clock_ms)*1000u;}
void multicore_reset_core1() {events+='C';}
namespace storage {
Config config;
bool state_ready() {return true;}
bool save() {events+='S'; ++saves; return save_ok;}
}
namespace display {void prepare_reboot() {events+='D';}}
namespace ble {void shutdown() {events+='B';}}
namespace usb {
bool disk_requested=false,save_requested=false,dirty=false;
bool disk_mode() {return disk;}
void transition_notice(const char *) {events+='L'; if(press_during_drain) wake_idle=false;}
void shutdown() {events+='U';}
}
namespace power::board {
WakeInfo capture_wake() {return {};}
bool reset_startup(bool) {return false;}
bool wake_inputs_idle() {return wake_idle;}
[[noreturn]] void enter(uint32_t duration) {events+='P'; throw Sleeping{duration};}
}
static void expect_sleep(uint32_t duration,const char *order) {
    assert(power::pending());
    try {power::process(); assert(false);}
    catch(const Sleeping &sleep) {assert(sleep.duration==duration);}
    assert(!power::pending() && events==order);
    events.clear();
}
int main() {
    power::init(); power::menu(true);
    assert(!power::command("status"));
    for(const char *line:{"power sleep 0","power sleep -1","power sleep 4294968","power sleep 1.5"}) {
        assert(power::command(line) && !power::pending());
    }
    assert(power::command("power idle 86401")); assert(storage::config.menu_sleep_seconds==300);
    assert(power::command("power idle 5")); assert(usb::dirty);
    clock_ms=4999; power::idle(false); assert(!power::pending());
    clock_ms=5000; power::idle(false); expect_sleep(0,"SLBDCUP");
    assert(!usb::dirty && saves==1);
    // Actual input resets idle; a held key continually prevents sleep.
    clock_ms=10000; power::idle(true); assert(!power::pending());
    clock_ms=14999; power::idle(false); assert(!power::pending());
    power::activity(); // Console input.
    clock_ms=19998; power::idle(false); assert(!power::pending());
    // Time spent in-game never contributes to the next menu's timeout.
    power::menu(false); clock_ms=1000000; power::idle(false); assert(!power::pending());
    power::menu(true); clock_ms+=4999; power::idle(false); assert(!power::pending());
    ++clock_ms; power::idle(false); expect_sleep(0,"LBDCUP");
    assert(power::command("power idle 0"));
    clock_ms+=1000000; power::idle(false); assert(!power::pending());
    usb::dirty=false;
    // Disk ownership/transition and asserted wake inputs reject without teardown.
    disk=true; power::command("power sleep"); assert(!power::pending());
    disk=false; usb::disk_requested=true; power::command("power sleep"); assert(!power::pending());
    usb::disk_requested=false; wake_idle=false; power::command("power sleep"); assert(!power::pending());
    wake_idle=true;
    // A save failure preserves pending changes and leaves display/USB running.
    usb::dirty=true; save_ok=false; power::command("power sleep 10");
    assert(power::pending()); power::process(); assert(!power::pending());
    assert(usb::dirty && events=="S"); events.clear();
    save_ok=true; power::command("power sleep 10"); expect_sleep(10000,"SLBDCUP");
    // A button pressed during log drain still cancels before shutdown.
    press_during_drain=true; power::command("power sleep"); power::process();
    assert(events=="L" && !power::pending()); events.clear();
    press_during_drain=false; wake_idle=true;
    power::command("power sleep 4294967"); expect_sleep(4294967000u,"LBDCUP");
    assert(power::command("power status"));
}
