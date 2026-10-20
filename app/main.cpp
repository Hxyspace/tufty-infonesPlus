#include "pico/stdlib.h"
#include "hardware/psram.h"
#include "display.hpp"
#include "storage.hpp"
#include "usb.hpp"
#include "menu.hpp"
#include "core_api/host.hpp"
#include "core_api/cartridge.hpp"
#include "presentation/frame_clock.hpp"
#include "presentation/pixels.hpp"
#include "power.hpp"
#include "ble.hpp"
#include "controllers.hpp"
#include "diagnostics.hpp"
#include "memory_stats.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
// Producer owns one of three SRAM frames; publishing exchanges ownership.
static presentation::GameFrame *SCREEN=nullptr;
static uint32_t work_started=0;
static presentation::FrameClock frame_clock;
static const uint8_t *rom_data=nullptr;
static size_t rom_bytes=0;
static bool usb_ready=false;
static bool ble_ready=false;
static void sample_local() {
    input::hub.local((!gpio_get(BTN_A)?input::LocalA:0)|(!gpio_get(BTN_B)?input::LocalB:0)|
        (!gpio_get(BTN_C)?input::LocalC:0)|(!gpio_get(BTN_UP)?input::LocalUp:0)|
        (!gpio_get(BTN_DOWN)?input::LocalDown:0)|(!gpio_get(BTN_HOME)?input::LocalHome:0));
}
void tufty_service() {
    static uint32_t previous=0;
    uint32_t now=time_us_32();
    static bool servicing=false;
    if(usb_ready && !servicing && now-previous>=1000) {
        tufty::memory::sample_stack(0);
        if(diagnostics::playing && previous)
            diagnostics::service_gap_max_us=std::max(diagnostics::service_gap_max_us,now-previous);
        servicing=true; previous=now; usb::poll();
        const auto usb_done=time_us_32();
        if(ble_ready) ble::poll();
        const auto ble_done=time_us_32();
        sample_local();
        if(diagnostics::playing){
            diagnostics::usb_max_us=std::max(diagnostics::usb_max_us,usb_done-now);
            diagnostics::ble_max_us=std::max(diagnostics::ble_max_us,ble_done-usb_done);
            if(diagnostics::working)diagnostics::service_work_us+=time_us_32()-now;
        }
        servicing=false;
    }
}
static void game_input(uint8_t &p1,uint8_t &p2,bool &quit) {
    tufty_service();
    sample_local();
    p1=input::hub.nes(storage::config.swap_ab);p2=0;
    quit=input::hub.home() || usb::disk_requested || usb::rescan_requested || power::pending();
}
static void __not_in_flash_func(game_line)(unsigned y,const uint16_t *pixels,uint8_t mask,bool pal) {
    SCREEN->line(y,pixels,mask,pal);
}
static void game_frame() {
    const auto publish_started=time_us_32();
    diagnostics::rate.frame(publish_started);
    SCREEN=&display::present_game();
    diagnostics::publish_last_us=time_us_32()-publish_started;
    diagnostics::publish_max_us=std::max(diagnostics::publish_max_us,diagnostics::publish_last_us);
    if(diagnostics::frames){
        diagnostics::work_last_us=time_us_32()-work_started;
        diagnostics::work_max_us=std::max(diagnostics::work_max_us,diagnostics::work_last_us);
    }
    diagnostics::service_last_us=diagnostics::service_work_us;
    diagnostics::working=false;
    ++diagnostics::frames;
    uint32_t target=frame_clock.next(time_us_32());
    while(static_cast<int32_t>(time_us_32()-target)<0) {tufty_service();tight_loop_contents();}
    work_started=time_us_32();
    diagnostics::service_work_us=0;
    diagnostics::working=true;
}
static void game_message(const char *text){std::printf("NES: %s\n",text);}
static void run_game() {
    nes::Cartridge image;const char *error=nullptr;
    if(!nes::inspect(rom_data,rom_bytes,image,&error)) {
        std::snprintf(storage::error,sizeof storage::error,"%s",error);return;
    }
    const auto now=time_us_32();
    if(image.timing==1)frame_clock.reset(now,1662607,341*312*5,16);
    else if(image.timing==3)frame_clock.reset(now,1773448,341*312,3);
    else frame_clock.reset(now,1789773,341*262,3);
    const nes::Host host{tufty_service,game_input,game_line,game_frame,game_message};
    SCREEN=&display::game_buffer();display::begin_game();
    diagnostics::reset();diagnostics::playing=true;
    if(!nes::infones_adapter::run(host,rom_data,rom_bytes,&error))
        std::snprintf(storage::error,sizeof storage::error,"%s",error?error:"Core failed");
    display::end_game();
    diagnostics::playing=false;
    diagnostics::working=false;
}
static void flush_config() {
    if(!usb::dirty && !usb::save_requested) return;
    usb::save_requested=false;
    bool ok=storage::save();
    if(ok) usb::dirty=false;
    std::printf("State save: %s\n",ok?"OK":"unavailable/failed; original record retained");
}
int main() {
    display::early_init();
    power::init();
    for(uint pin : {uint(BTN_A),uint(BTN_B),uint(BTN_C),uint(BTN_UP),uint(BTN_DOWN),uint(BTN_HOME),uint(BW_RESET_SW)}) {
        gpio_init(pin); gpio_set_dir(pin,GPIO_IN); gpio_pull_up(pin);
    }
    gpio_init(BW_SW_POWER_EN); gpio_set_dir(BW_SW_POWER_EN,GPIO_OUT); gpio_put(BW_SW_POWER_EN,1);
    gpio_init(CYW43_DEFAULT_PIN_WL_REG_ON); gpio_set_dir(CYW43_DEFAULT_PIN_WL_REG_ON,GPIO_OUT); gpio_put(CYW43_DEFAULT_PIN_WL_REG_ON,0);
    for(uint pin=0;pin<4;++pin) {gpio_init(pin); gpio_set_dir(pin,GPIO_OUT); gpio_put(pin,0);}
    bool disk=usb::consume_disk_request();
    disk=power::reset_startup() || disk;
    usb::init(disk); usb_ready=true;
    if(psram_get_size()<8*1024*1024) panic("8 MiB PSRAM required");
    display::start();
    storage::init(!disk);
    controllers::init();
    display::brightness(storage::config.brightness);
    if(disk) {
        usb::acquire_disk(storage::mounted()); usb::run_disk();
    }
    ble::init();
    ble_ready=true;
    storage::scan(); menu::load_art();
    int selected=0;
    for(size_t i=0;i<storage::count;++i) if(!std::strcmp(storage::entries[i].name,storage::config.last_rom)) selected=i;
    float position=selected;
    unsigned back=1;
    uint32_t opened=to_ms_since_boot(get_absolute_time());
    sample_local(); input::hub.clear_events();
    power::menu(true);
    while(true) {
        tufty_service();
        if(usb::disk_requested) {
            flush_config();
            if(usb::dirty) {usb::disk_requested=false; std::printf("Disk entry blocked: unsaved State\n");}
            else usb::reboot_disk();
        }
        if(power::pending()) power::process();
        if(usb::save_requested) flush_config();
        if(usb::rescan_requested) {
            usb::rescan_requested=false; storage::scan(); menu::cover(-1);
            selected=storage::count?std::min<int>(selected,storage::count-1):0; position=selected;
        }
        auto keys=input::hub.take_menu();
        power::idle(keys.held || keys.pressed || input::hub.home());
        if(power::pending()) {power::process(); continue;}
        uint8_t pressed=keys.pressed;
        if(pressed&input::Previous) selected=std::max(0,selected-1);
        if(pressed&input::Next) selected=std::min<int>(std::max<int>(0,storage::count-1),selected+1);
        if(storage::count && (pressed&(input::Previous|input::Next))) storage::error[0]=0;
        if(pressed&input::Confirm && storage::count) {
            input::hub.clear_events();
            power::menu(false);
            storage::error[0]=0;
            if(storage::load(selected,rom_data,rom_bytes)) {
                if(std::strcmp(storage::config.last_rom,storage::entries[selected].name)) {
                    std::snprintf(storage::config.last_rom,sizeof(storage::config.last_rom),"%s",storage::entries[selected].name);
                    usb::dirty=storage::state_ready(); flush_config();
                }
                run_game();
            }
            input::hub.clear_events();
            power::menu(true);
            opened=to_ms_since_boot(get_absolute_time());
        }
        menu::cover(selected);
        position+=(selected-position)*0.3f;
        if(std::fabs(selected-position)<0.005f) position=selected;
        uint32_t now=to_ms_since_boot(get_absolute_time());
        menu::draw(display::menu_buffers[back],selected,position,now-opened,storage::error[0]?storage::error:nullptr);
        display::present_menu(back); back^=1;
    }
}
