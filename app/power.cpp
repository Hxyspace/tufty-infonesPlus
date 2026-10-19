#include "power.hpp"
#include "power_policy.hpp"
#include "power_board.hpp"
#include "storage.hpp"
#include "usb.hpp"
#include "display.hpp"
#include "ble.hpp"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <cstdio>
#include <cstring>

namespace power {
static_assert(storage::Config{}.menu_sleep_seconds==kDefaultIdleSeconds);
namespace {
IdleTimer timer;
Request request;
board::WakeInfo wake;
const char *blocked="NONE";
uint32_t now() {return to_ms_since_boot(get_absolute_time());}
const char *error_text(Error error) {
    switch(error) {
        case Error::None: return "NONE";
        case Error::Busy: return "BUSY";
        case Error::Unsaved: return "UNSAVED_STATE";
        case Error::Backend: return "BACKEND";
    }
    return "BACKEND";
}
bool ready() {
    blocked=usb::disk_mode()?"USB_DISK":usb::disk_requested?"USB_TRANSITION":
        !board::wake_inputs_idle()?"WAKE_INPUT":"NONE";
    return !std::strcmp(blocked,"NONE");
}
bool save() {
    if(!usb::dirty && !usb::save_requested) return true;
    usb::save_requested=false;
    if(!storage::save()) return false;
    usb::dirty=false; return true;
}
Error enter(uint32_t duration_ms) {
    usb::transition_notice("POWER SLEEP: ENTERING; wake restarts the firmware\r\n");
    // USB draining may take time. Check inputs once more before any teardown.
    if(!ready()) return Error::Busy;
    ble::shutdown();
    display::prepare_reboot();
    multicore_reset_core1(); // No PSRAM/DMA owner may survive pin/clock shutdown.
    usb::shutdown();
    board::enter(duration_ms);
}
const Hooks hooks{ready,save,enter};
const char *wake_reason() {
    if(wake.watchdog) return "WATCHDOG";
    if(wake.from_sleep) {
        if(wake.sources&board::kWakeVbus) return "VBUS";
        if(wake.sources&board::kWakeRtc) return "RTC";
        if(wake.sources&board::kWakeButtons) return "BUTTON";
        if(wake.sources&board::kWakeAlarm) return "ALARM";
    }
    return wake.reset?"RESET":"POWER_ON";
}
void report() {
    unsigned buttons=0;
    constexpr unsigned pins[]={BTN_A,BTN_B,BTN_C,BTN_UP,BTN_DOWN};
    for(unsigned i=0;i<5;++i) if(wake.button_gpios&(1u<<pins[i])) buttons|=1u<<i;
    std::printf("POWER STATUS: state=%s error=%s blocked=%s wake=%s buttons=0x%02x idle_seconds=%lu duration_ms=%lu\n",
        request.pending()?"REQUESTED":request.error()==Error::None?"READY":"FAILED",
        error_text(request.error()),blocked,wake_reason(),buttons,
        static_cast<unsigned long>(storage::config.menu_sleep_seconds),
        static_cast<unsigned long>(request.duration()));
    if(wake.entry_failed) std::printf("POWER DIAG: entry_rc=%ld state=0x%08lx pending=0x%02lx\n",
        static_cast<long>(wake.entry_rc),static_cast<unsigned long>(wake.entry_state),
        static_cast<unsigned long>(wake.entry_pending));
}
void queue(uint32_t duration_ms,bool automatic=false) {
    bool accepted=request.queue(duration_ms,hooks);
    std::printf("POWER SLEEP: %s mode=%s source=%s error=%s blocked=%s\n",
        accepted?"REQUESTED":"REJECTED",duration_ms?"TIMED":"BUTTON",automatic?"IDLE":"CONSOLE",
        accepted?"NONE":error_text(request.pending()?Error::Busy:request.error()),blocked);
}
}
void init() {wake=board::capture_wake(); timer.activity(now());}
bool reset_startup() {return board::reset_startup(wake.watchdog);}
void activity() {timer.activity(now());}
void menu(bool enabled) {timer.menu(enabled,now());}
bool pending() {return request.pending();}
void idle(bool input) {
    if(input) activity();
    if(!request.pending() && timer.due(now(),storage::config.menu_sleep_seconds)) {
        // Failed/rejected automatic attempts retry only after another interval.
        activity(); queue(0,true);
    }
}
void process() {
    if(!request.pending()) return;
    request.run(hooks);
    activity();
    report();
}
bool command(const char *line) {
    if(!std::strcmp(line,"power status")) report();
    else if(!std::strcmp(line,"power sleep")) queue(0);
    else if(!std::strncmp(line,"power sleep ",12)) {
        uint32_t value;
        if(!seconds(line+12,kMaxSleepSeconds,value) || !value)
            std::printf("POWER SLEEP: INVALID_DURATION seconds=1..4294967\n");
        else queue(value*1000u);
    } else if(!std::strncmp(line,"power idle ",11)) {
        uint32_t value;
        if(!seconds(line+11,kMaxIdleSeconds,value)) std::printf("POWER IDLE: INVALID_DURATION seconds=0..86400\n");
        else {
            storage::config.menu_sleep_seconds=value;
            usb::dirty=storage::state_ready(); activity();
            std::printf("POWER IDLE: seconds=%lu; use save to persist\n",static_cast<unsigned long>(value));
        }
    } else if(!std::strncmp(line,"power",5)) std::printf("power status | power sleep [seconds] | power idle 0..86400\n");
    else return false;
    return true;
}
}
