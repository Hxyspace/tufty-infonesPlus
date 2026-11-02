#include "pico/stdlib.h"
#include "hardware/psram.h"
#include "display.hpp"
#include "disk_display.hpp"
#include "storage.hpp"
#include "usb.hpp"
#include "menu.hpp"
#include "game_lists.hpp"
#include "ui.hpp"
#include "settings.hpp"
#include "configuration.hpp"
#include "menu_repeat.hpp"
#include "core_api/host.hpp"
#include "core_api/cartridge.hpp"
#include "presentation/frame_clock.hpp"
#include "presentation/pixels.hpp"
#include "power.hpp"
#include "battery.hpp"
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
static bool library_loading=false;
static uint32_t library_draw_at=0;
static unsigned library_back=0;
static void library_progress() {
    const auto now=to_ms_since_boot(get_absolute_time());
    if(library_loading && now-library_draw_at<33) return;
    library_loading=true; library_draw_at=now;
    menu::draw_loading(display::menu_buffers[library_back],now);
    display::present_menu(library_back); library_back^=1;
}
static void refresh_library(bool force=false) {
    library_loading=false; menu::select(-1);
    game_lists::view(game_lists::View::All);
    storage::scan(force,library_progress);
    game_lists::refresh();
    input::hub.clear_events();
}
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
    const auto pad=input::hub.gamepad();
    p1=input::nes(pad,storage::config.swap_ab,to_ms_since_boot(get_absolute_time()));p2=0;
    quit=((pad.controller.keys|pad.pressed)&gamepad::KeyHome) || usb::disk_requested || usb::boot_requested || usb::rescan_requested || power::pending();
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
static uint8_t game_timing=0;
static void game_started() {
    game_lists::started();
    const auto now=time_us_32();
    if(game_timing==1)frame_clock.reset(now,1662607,341*312*5,16);
    else if(game_timing==3)frame_clock.reset(now,1773448,341*312,3);
    else frame_clock.reset(now,1789773,341*262,3);
}
static void run_game(int index) {
    nes::Cartridge image;const char *error=nullptr;
    if(!nes::inspect(rom_data,rom_bytes,image,&error)) {
        std::printf("NES: %s\n",error?error:"Unsupported ROM");
        std::snprintf(storage::error,sizeof storage::error,"Unsupported ROM");return;
    }
    if(!menu::suspend(index)) return;
    game_timing=image.timing;
    const nes::Host host{tufty_service,game_input,game_line,game_frame,game_message,game_started};
    SCREEN=&display::game_buffer();display::begin_game(storage::config.palette);
    diagnostics::reset();diagnostics::playing=true;
    if(!nes::infones_adapter::run(host,rom_data,rom_bytes,&error)) {
        std::printf("NES: %s\n",error?error:"Core failed");
        std::snprintf(storage::error,sizeof storage::error,"%s",
            error && std::strstr(error,"alloc")?"Out of memory":"Core failed");
    }
    display::end_game();
    diagnostics::playing=false;
    diagnostics::working=false;
    menu::resume();
}
int main() {
    display::early_init();
    power::init();
    battery::init();
    for(uint pin : {uint(BTN_A),uint(BTN_B),uint(BTN_C),uint(BTN_UP),uint(BTN_DOWN),uint(BTN_HOME),uint(BW_RESET_SW)}) {
        gpio_init(pin); gpio_set_dir(pin,GPIO_IN); gpio_pull_up(pin);
    }
    gpio_init(BW_SW_POWER_EN); gpio_set_dir(BW_SW_POWER_EN,GPIO_OUT); gpio_put(BW_SW_POWER_EN,1);
    gpio_init(CYW43_DEFAULT_PIN_WL_REG_ON); gpio_set_dir(CYW43_DEFAULT_PIN_WL_REG_ON,GPIO_OUT); gpio_put(CYW43_DEFAULT_PIN_WL_REG_ON,0);
    for(uint pin=0;pin<4;++pin) {gpio_init(pin); gpio_set_dir(pin,GPIO_OUT); gpio_put(pin,0);}
    bool disk=usb::consume_disk_request();
    disk=power::reset_startup() || disk;
    usb::init(disk); usb_ready=true;
    if(disk) {
        // Only System and the State change flag are needed in this boot.
        // USB tasks start only after core1 is safely running in RAM.
        disk_display::start(storage::config.brightness);
        usb::acquire_disk(storage::init_disk());
        usb::run_disk();
    }
    if(psram_get_size()<8*1024*1024) panic("8 MiB PSRAM required");
    display::start();
    storage::init();
    configuration::init();
    controllers::init();
    display::brightness(storage::config.brightness);
    ble::init();
    ble_ready=true;
    menu::load_art(); refresh_library();
    int selected=library_loading?0:storage::count?1:0;
    uint32_t sleep_duration=0;
    menu::Repeat repeat;
    float position=selected;
    unsigned back=1;
    uint32_t opened=to_ms_since_boot(get_absolute_time());
    sample_local(); input::hub.clear_events();
    power::menu(true);
    menu::enter(opened);
    menu::select(game_lists::game(selected-1));
    while(true) {
        tufty_service();
        ui::service(to_ms_since_boot(get_absolute_time()));
        configuration::sync();
        game_lists::refresh();
        if(selected>menu::last_card()) {selected=menu::last_card(); position=selected;}
        if(!settings::transitioning()) {
            sleep_duration=0;
            if(usb::disk_requested || usb::boot_requested) {
                const bool disk=usb::disk_requested;
                usb::disk_requested=usb::boot_requested=false;
                settings::request_exit(disk?settings::Exit::Disk:settings::Exit::Bootsel);
            } else if(power::pending()) {
                sleep_duration=power::duration(); power::cancel();
                settings::request_exit(settings::Exit::Sleep);
            }
        }
        if(usb::save_requested && !settings::transitioning()) settings::request_save();
        if(usb::rescan_requested) {
            usb::rescan_requested=false; refresh_library(true); selected=0;
            position=selected; menu::reset_page();
            menu::select(game_lists::game(selected-1));
        }
        const auto pad=input::hub.gamepad();
        auto keys=input::menu(pad);
        const bool home=pad.pressed&gamepad::KeyHome;
        // The dispatcher now owns these events; downstream service calls may
        // publish the next batch without having their edges cleared afterward.
        input::hub.acknowledge(pad);
        if(!settings::transitioning()) power::idle(keys.held || keys.pressed || home || input::hub.home());
        if(settings::active()) {
            settings::update(keys,home,to_ms_since_boot(get_absolute_time()));
            settings::draw(display::menu_buffers[back]);
            display::present_menu(back); back^=1;
            settings::after_present();
            const auto exit=settings::take_exit();
            if(exit==settings::Exit::Rebuild) refresh_library(true);
            if(exit==settings::Exit::Disk) usb::reboot_disk();
            else if(exit==settings::Exit::Bootsel) usb::reboot_bootsel();
            else if(exit==settings::Exit::Defaults) usb::reboot();
            else if(exit==settings::Exit::Sleep) {
                power::sleep(sleep_duration); sleep_duration=0; power::process(true);
            }
            if(exit!=settings::Exit::None) {
                if(exit!=settings::Exit::ResumeLibrary) {selected=exit==settings::Exit::Browse?1:0; position=selected;}
                repeat.reset(); menu::select(game_lists::game(selected-1));
                opened=to_ms_since_boot(get_absolute_time()); power::activity();
            }
            continue;
        }
        if(power::pending()) continue;
        uint8_t pressed=repeat.poll(keys,to_ms_since_boot(get_absolute_time()));
        const int old_selected=selected;
        if(pressed&input::Previous) selected=std::max(0,selected-1);
        if(pressed&input::Next) selected=std::min(menu::last_card(),selected+1);
        menu::actions(pressed,selected,position);
        if(selected!=old_selected) storage::error[0]=0;
        if(selected>0 && !(pressed&(input::Previous|input::Next))) {
            if(pressed&input::PagePrevious) menu::previous_page();
            if(pressed&input::PageNext) menu::next_page();
        }
        if(menu::launch_requested(pressed) && selected==0) {
            settings::open(); repeat.reset(); continue;
        }
        const int game=game_lists::game(selected-1);
        if(menu::launch_requested(pressed) && game>=0) {
            if(!game_lists::save()) {settings::request_save(); repeat.reset(); continue;}
            repeat.reset();
            input::hub.clear_events();
            power::menu(false);
            storage::error[0]=0;
            if(storage::load(size_t(game),rom_data,rom_bytes)) {
                run_game(game);
            }
            selected=game_lists::position(game)+1; position=selected;
            input::hub.clear_events();
            power::menu(true);
            opened=to_ms_since_boot(get_absolute_time());
            menu::enter(opened);
        }
        const auto prepare_started=time_us_32();
        menu::select(game_lists::game(selected-1));
        menu::timing.prepare_us=time_us_32()-prepare_started;
        menu::timing.prepare_max_us=std::max(menu::timing.prepare_max_us,menu::timing.prepare_us);
        position+=(selected-position)*0.3f;
        if(std::fabs(selected-position)<0.005f) position=selected;
        uint32_t now=to_ms_since_boot(get_absolute_time());
        const auto draw_started=time_us_32();
        menu::draw(display::menu_buffers[back],selected,position,now-opened,storage::error[0]?storage::error:nullptr);
        menu::timing.draw_us=time_us_32()-draw_started;
        menu::timing.draw_max_us=std::max(menu::timing.draw_max_us,menu::timing.draw_us);
        display::present_menu(back); back^=1;
        menu::timing.frame_us=time_us_32()-prepare_started;
        menu::timing.frame_max_us=std::max(menu::timing.frame_max_us,menu::timing.frame_us);
        ++menu::timing.frames;
        game_lists::idle(now,keys.held || keys.pressed || position!=selected);
        const auto prefetch_started=time_us_32();
        if(!usb::disk_requested && !usb::boot_requested && !usb::rescan_requested && !power::pending() &&
           menu::load_current_cover()) {
            menu::timing.cover_load_us=time_us_32()-prefetch_started;
            menu::timing.cover_load_max_us=std::max(menu::timing.cover_load_max_us,menu::timing.cover_load_us);
        } else if(!usb::disk_requested && !usb::boot_requested && !usb::rescan_requested && !power::pending() &&
                  menu::prefetch(to_ms_since_boot(get_absolute_time()),keys.held || keys.pressed)) {
            menu::timing.prefetch_us=time_us_32()-prefetch_started;
            menu::timing.prefetch_max_us=std::max(menu::timing.prefetch_max_us,menu::timing.prefetch_us);
        }
    }
}
