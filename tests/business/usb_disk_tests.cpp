// Exercise production MSC callbacks and the USB/UI loop without a device.
#include "usb.hpp"
#include "storage.hpp"
#include "display.hpp"
#include "disk_ui.hpp"
#include "fat_disk.hpp"
#include "pico/stdlib.h"
#include "tusb.h"
#include "memory_stats.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

extern "C" {
int32_t tud_msc_read10_cb(uint8_t,uint32_t,uint32_t,void *,uint32_t);
int32_t tud_msc_write10_cb(uint8_t,uint32_t,uint32_t,uint8_t *,uint32_t);
void tud_msc_read10_complete_cb(uint8_t);
void tud_msc_write10_complete_cb(uint8_t);
}
namespace {
struct Stop {};
uint32_t clock_us=0,deadline=1600000,last_poll=0,max_gap=0,frame_due=0,next_io=0,last_io=0;
unsigned polls=0,reads=0,frames=0,writes=0,sleeps=0,light_changes=0,cancelled=0;
unsigned transfer_frames=0;
unsigned te_changes=0;
bool requested_te=true;
disk_ui::State rendered=disk_ui::State::Unavailable,front=disk_ui::State::Unavailable;
bool stopped=false,held=false,write_ok=true,pumping=true,lights=false;
bool inject_write=false,injected=false;
bool idle_during_reads=false;
uint8_t flash[8192]{};
std::vector<bool> pauses;
bool read(uint32_t at,void *out,uint32_t bytes) {
    assert(at+bytes<=sizeof(flash)); std::memcpy(out,flash+at,bytes); return true;
}
void run_until(uint32_t until) {
    deadline=until;
    try {usb::run_disk(); assert(false);} catch(const Stop &) {}
}
}
absolute_time_t get_absolute_time() {return clock_us;}
uint32_t time_us_32() {return clock_us;}
void sleep_ms(uint32_t ms) {++sleeps; clock_us+=ms*1000;}
void tight_loop_contents() {
    clock_us+=50;
    if(clock_us>=frame_due) front=rendered;
}
void gpio_put(uint pin,bool value) {
    if(pin==BW_LED_0) {assert(lights!=value); ++light_changes; lights=value;}
}
[[noreturn]] void watchdog_reboot(uint32_t,uint32_t,uint32_t) {assert(false); throw Stop{};}
[[noreturn]] void reset_usb_boot(uint32_t,uint32_t) {assert(false); throw Stop{};}
void tud_task() {
    if(clock_us>=deadline) throw Stop{};
    max_gap=std::max(max_gap,clock_us-last_poll); last_poll=clock_us; ++polls;
    clock_us+=50; // Time advances without depending on firmware sleeps.
    if(!pumping) return;
    if(clock_us<200000 && clock_us>=next_io) {
        uint8_t out[512];
        assert(tud_msc_read10_cb(0,0,0,out,sizeof(out))==sizeof(out));
        tud_msc_read10_complete_cb(0); ++reads;
        assert(!lights && held && !stopped && transfer_frames==0);
        // Opening directories freezes animation without write status or LEDs.
        next_io=clock_us+4000; last_io=clock_us/1000*1000;
    }
    if(inject_write || (injected && clock_us<1000000 && clock_us>=next_io)) {
        inject_write=false; injected=true;
        uint8_t data[4096]; std::memcpy(data,flash,sizeof(data)); data[0]^=1;
        assert(tud_msc_write10_cb(0,0,0,data,sizeof(data))==sizeof(data));
        tud_msc_write10_complete_cb(0);
        assert(lights && held && stopped && front==disk_ui::State::Transferring);
        next_io=clock_us+4000; last_io=clock_us/1000*1000;
    }
    if(injected && clock_us>=1000000 && clock_us<1550000 && clock_us>=next_io) {
        unsigned changes=light_changes,status_frames=transfer_frames;
        uint8_t out[512];
        assert(tud_msc_read10_cb(0,0,0,out,sizeof(out))==sizeof(out));
        tud_msc_read10_complete_cb(0);
        assert(light_changes==changes && transfer_frames==status_frames);
        if(clock_us>=1530000) assert(!lights && front==disk_ui::State::Ready);
        next_io=clock_us+4000; last_io=clock_us/1000*1000;
    }
}
namespace power {
bool pending() {return false;}
bool command(const char *) {return false;}
void activity() {}
}
namespace ble {bool command(const char *) {return false;}}
namespace tufty::memory {Stats stats(){return {};}}
namespace controllers {bool command(const char *) {return false;} void set_controller_id(uint16_t) {}}
namespace display {
Stats stats(){return {};}
static uint16_t buffers[2][1];
uint16_t *menu_buffers[]={buffers[0],buffers[1]};
void brightness(uint8_t) {}
void te_sync(bool value) {++te_changes;requested_te=value;}
bool te_requested() {return requested_te;}
void prepare_reboot() {assert(false);}
bool paused() {return stopped;}
void pause(bool value) {pauses.push_back(value); stopped=value;}
void hold_disk(bool value) {held=value;}
bool disk_ready() {return clock_us>=frame_due;}
void present_disk(unsigned) {
    assert(disk_ready() && !held && !stopped);
    assert(lights==(rendered==disk_ui::State::Transferring));
    if(lights) ++transfer_frames;
    frame_due=clock_us+25000; ++frames;
}
}
namespace disk_ui {
bool draw(uint16_t *,State state,uint32_t,Service service) {
    assert(!held && !stopped);
    if(state!=State::Ready) {
        assert(!service);
        rendered=state;
        return true;
    }
    assert(!lights);
    if(!service) {
        // First frame, or returning to idle while background reads continue.
        if(injected) idle_during_reads=true;
        rendered=state;
        return true;
    }
    assert(service && clock_us-last_io>=500000);
    for(int i=0;i<8;++i) {
        clock_us+=200;
        // Inject a write halfway through rendering an idle frame.
        if(!injected && clock_us>=700000 && i==3) inject_write=true;
        if(!service()) {++cancelled; return false;}
    }
    rendered=state;
    return true;
}
}
namespace storage {
Config config;
size_t count=0;
bool mounted() {return true;}
bool state_ready() {return true;}
bool unmount_system() {flashfat::disk.runtime_owned=false; return true;}
bool replace_system_sector(uint32_t at,const uint8_t *data) {
    assert(display::paused() && held && !flashfat::disk.runtime_owned);
    assert(front==disk_ui::State::Transferring); // Visible before Flash programming.
    ++writes;
    if(!write_ok) return false;
    assert(at+4096<=sizeof(flash)); std::memcpy(flash+at,data,4096); return true;
}
}
int main() {
    // Feed real CDC0/Console dispatch. The switch is runtime-only; query and
    // malformed commands must not change it or mark persistent State dirty.
    pumping=false;usb::init(false);usb_test::console_connected=true;
    const auto console=[](const char *text){usb_test::console_input=text;usb::poll();};
    console("lcd te\n");assert(te_changes==0 && requested_te);
    console("lcd te off\n");assert(te_changes==1 && !requested_te && !usb::dirty);
    console("lcd te off extra\n");assert(te_changes==1 && !requested_te);
    console("lcd te\n");assert(te_changes==1 && !requested_te);
    console("lcd te on\n");assert(te_changes==2 && requested_te && !usb::dirty);
    console("lcd te on\n");assert(te_changes==3 && requested_te); // Explicit retry.
    usb_test::console_connected=false;
    clock_us=last_poll=max_gap=polls=0;pumping=true;
    flashfat::disk.read=read; flashfat::disk.sector_size=4096;
    flashfat::disk.runtime_owned=true;
    usb::init(true); usb::acquire_disk(true);
    assert(pauses.empty());
    run_until(2200000);
    assert(polls>10000 && reads>=40 && max_gap<=26000 && frames>=3 && !sleeps);
    assert(cancelled==1 && writes>50);
    assert(transfer_frames==1 && idle_during_reads); // Writes alone change status.
    assert((pauses==std::vector<bool>{true,false})); // One park for the write burst.
    assert(light_changes==2 && !lights && !held && !stopped); // One write burst, no flicker.
    pumping=false; pauses.clear();
    uint8_t data[4096]; std::memcpy(data,flash,sizeof(data));
    unsigned before=writes;
    assert(tud_msc_write10_cb(0,0,0,data,sizeof(data))==sizeof(data));
    tud_msc_write10_complete_cb(0);
    assert(lights && held && pauses.empty() && writes==before); // No redundant erase.
    last_io=clock_us/1000*1000;
    run_until(clock_us+499000);
    assert(lights && held && frames>=3);
    run_until(last_io+501000);
    assert(!lights && !held && pauses.empty());

    // A Flash error is rendered after releasing the park owned by disk mode.
    write_ok=false; data[12]^=1;
    assert(tud_msc_write10_cb(0,0,0,data,sizeof(data))==-1);
    assert(stopped);
    run_until(clock_us+50000);
    assert((pauses==std::vector<bool>{true,false}) && !stopped && !held && !lights);
}
