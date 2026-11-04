// Real MSC callbacks: storage and status updates never wait for the renderer.
#include "usb.hpp"
#include "storage.hpp"
#include "display.hpp"
#include "disk_display.hpp"
#include "fat_disk.hpp"
#include "pico/stdlib.h"
#include "tusb.h"
#include "memory_stats.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>

extern "C" {
int32_t tud_msc_read10_cb(uint8_t,uint32_t,uint32_t,void *,uint32_t);
int32_t tud_msc_write10_cb(uint8_t,uint32_t,uint32_t,uint8_t *,uint32_t);
void tud_msc_write10_complete_cb(uint8_t);
bool tud_msc_start_stop_cb(uint8_t,uint8_t,bool,bool);
void tud_msc_scsi_complete_cb(uint8_t,const uint8_t[16]);
}
namespace {
struct Stop {};
uint32_t clock_us=0,deadline=0,last_poll=0,max_gap=0,next_io=0,last_write=0;
unsigned polls=0,reads=0,writes=0,light_changes=0,te_changes=0;
unsigned markers=0;
bool marker_ok=true;
bool pumping=false,lights=false,write_ok=true,requested_te=true,rebooted=false,prepared=false;
disk_ui::State state=disk_ui::State::Unavailable;
uint8_t flash[8192]{};
bool read(uint32_t at,void *out,uint32_t bytes) {
    assert(at+bytes<=sizeof flash);std::memcpy(out,flash+at,bytes);return true;
}
void run_until(uint32_t until) {
    deadline=until;
    try {usb::run_disk();assert(false);} catch(const Stop &) {}
}
}
absolute_time_t get_absolute_time() {return clock_us;}
uint32_t time_us_32() {return clock_us;}
void sleep_ms(uint32_t ms) {clock_us+=ms*1000;}
void tight_loop_contents() {clock_us+=50;}
void gpio_put(uint pin,bool value) {
    if(pin==BW_LED_0){assert(lights!=value);++light_changes;lights=value;}
}
[[noreturn]] void watchdog_reboot(uint32_t,uint32_t,uint32_t) {
    assert(prepared);rebooted=true;throw Stop{};
}
[[noreturn]] void reset_usb_boot(uint32_t,uint32_t) {assert(false);throw Stop{};}
void tud_task() {
    if(clock_us>=deadline)throw Stop{};
    max_gap=std::max(max_gap,clock_us-last_poll);last_poll=clock_us;++polls;clock_us+=50;
    if(!pumping || clock_us<next_io)return;
    if(clock_us>=200000 && clock_us<800000){
        uint8_t data[4096];std::memcpy(data,flash,sizeof data);data[0]^=1;
        assert(tud_msc_write10_cb(0,0,0,data,sizeof data)==sizeof data);
        tud_msc_write10_complete_cb(0);last_write=clock_us/1000*1000;
        assert(lights && state==disk_ui::State::Transferring);
    } else {
        uint8_t out[512];assert(tud_msc_read10_cb(0,0,0,out,sizeof out)==sizeof out);++reads;
        if(clock_us<200000)assert(!lights && state==disk_ui::State::Ready);
    }
    next_io=clock_us+4000;
}
namespace power {bool pending(){return false;}bool command(const char *){return false;}void activity(){}}
namespace ble {bool command(const char *){return false;}}
namespace tufty::memory {Stats stats(){return {};}}
namespace controllers {bool command(const char *){return false;}void set_controller_id(uint16_t){}}
namespace display {
Stats stats(){return {};}
void brightness(uint8_t){}
void te_sync(bool value){++te_changes;requested_te=value;}
bool te_requested(){return requested_te;}
void prepare_reboot(){prepared=true;}
// Any attempt to use the game renderer or pause the disk core fails the test.
bool paused(){assert(false);return false;}
void pause(bool){assert(false);}
}
namespace disk_display {
void state(disk_ui::State value){::state=value;}
bool active(){return true;}
}
namespace storage {
bool inspect_path(const char *,PathInfo &){assert(false);return false;}
bool list_directory(const char *,DirectoryVisitor,void *){assert(false);return false;}
bool read_file(const char *,void *,size_t,size_t &){assert(false);return false;}
bool read_file_prefix(const char *,void *,size_t,size_t &){assert(false);return false;}
bool mark_library_changed(){++markers;return marker_ok;}
Config config;
size_t count=0;
bool mounted(){return true;}
bool state_ready(){return true;}
bool unmount_system(){flashfat::disk.runtime_owned=false;return true;}
bool replace_system_sector(uint32_t at,const uint8_t *data){
    assert(markers==1 && marker_ok);
    assert(!flashfat::disk.runtime_owned && lights && state==disk_ui::State::Transferring);
    ++writes;clock_us+=40000; // A synchronous erase/program; no display callback.
    if(!write_ok)return false;
    assert(at+4096<=sizeof flash);std::memcpy(flash+at,data,4096);return true;
}
}
int main(int argc,char **argv){
    deadline=100000;usb::init(false);usb_test::console_connected=true;
    const auto console=[](const char *text){usb_test::console_input=text;usb::poll();};
    console("lcd te\n");assert(te_changes==0 && requested_te);
    console("lcd te off\n");assert(te_changes==1 && !requested_te && !usb::dirty);
    console("lcd te off extra\n");assert(te_changes==1 && !requested_te);
    console("lcd te\n");assert(te_changes==1 && !requested_te);
    console("lcd te on\n");assert(te_changes==2 && requested_te && !usb::dirty);
    console("lcd te on\n");assert(te_changes==3 && requested_te); // Explicit retry.
    console("bootsel\n");assert(usb::boot_requested);usb::boot_requested=false;
    usb_test::console_connected=false;
    flashfat::disk.read=read;flashfat::disk.sector_size=4096;flashfat::disk.runtime_owned=true;
    usb::init(true);usb::acquire_disk(true);pumping=true;
    if(argc>1 && !std::strcmp(argv[1],"marker_failure")) {
        marker_ok=false; uint8_t data[512]{}; data[0]=1;
        assert(tud_msc_write10_cb(0,0,0,data,sizeof data)==-1);
        assert(markers==1 && writes==0 && flash[0]==0);
        return 0;
    }
    run_until(1500000);
    assert(polls>5000 && reads>80 && writes>10 && max_gap<=40100);
    assert(light_changes==2 && !lights && state==disk_ui::State::Ready);
    assert(clock_us-last_write>=500000);
    pumping=false;
    uint8_t data[4096];std::memcpy(data,flash,sizeof data);
    const auto before=writes;
    assert(tud_msc_write10_cb(0,0,0,data,sizeof data)==sizeof data);
    assert(lights && writes==before); // Equal data skips erase, but reports activity.
    run_until(clock_us+600000);
    assert(lights && state==disk_ui::State::Transferring); // Still in flight.
    tud_msc_write10_complete_cb(0);last_write=clock_us/1000*1000;
    run_until(last_write+499000);assert(lights);
    uint8_t out[512];assert(tud_msc_read10_cb(0,0,0,out,sizeof out)==sizeof out);
    run_until(last_write+501000);assert(!lights && state==disk_ui::State::Ready);
    assert(tud_msc_write10_cb(0,UINT32_MAX,0,data,sizeof data)==-1 && writes==before);
    if(argc>1 && !std::strcmp(argv[1],"eject")){
        assert(tud_msc_start_stop_cb(0,0,false,true));
        uint8_t cmd[16]{SCSI_CMD_START_STOP_UNIT};tud_msc_scsi_complete_cb(0,cmd);
        const auto ejected=clock_us;
        run_until(ejected+249000);assert(!rebooted && state==disk_ui::State::Ejected);
        run_until(ejected+300000);assert(rebooted && prepared && !lights);
    } else {
        write_ok=false;data[12]^=1;
        assert(tud_msc_write10_cb(0,0,0,data,sizeof data)==-1);
        run_until(clock_us+1000);assert(!lights && state==disk_ui::State::Failed);
        assert(tud_msc_read10_cb(0,0,0,out,sizeof out)==-1);
    }
}
