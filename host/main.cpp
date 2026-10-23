#include "runtime.hpp"
#include "menu.hpp"
#include "menu_repeat.hpp"
#include "settings.hpp"
#include "configuration.hpp"
#include "controllers.hpp"
#include "ble.hpp"
#include "disk_ui.hpp"
#include "ui.hpp"
#include "core_api/host.hpp"
#include "core_api/cartridge.hpp"
#include "presentation/frame_clock.hpp"
#include "presentation/pixels.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace preview {
namespace {
SDL_Window *window=nullptr;
SDL_Renderer *renderer=nullptr;
SDL_Texture *texture=nullptr;
std::array<uint16_t,320*240> screen{};
// Screenshots always capture the last completed frame, even during a scanline callback.
std::array<uint16_t,320*240> presented{};
std::array<bool,SDL_NUM_SCANCODES> keys{};
bool running=true,initialized=false,scripted=false,playing=false;
bool disk_mode=false,disk_redraw=false;
disk_ui::State disk_state=disk_ui::State::Ready;
uint32_t disk_draw_at=0,disk_ejected_at=0;
uint64_t tick=0,started=0,max_ticks=0,game_frames=0;
int selected=0;
std::filesystem::path script_directory;
struct Action {uint64_t tick; std::string command,arg;};
std::vector<Action> actions;
size_t next_action=0;
std::exception_ptr callback_error;
constexpr const char *disk_states[]={"ready","transferring","unavailable","failed","ejected"};
void set_disk_state(disk_ui::State state) {
    disk_state=state; disk_redraw=true;
    if(state==disk_ui::State::Ejected) disk_ejected_at=micros()/1000;
}
void enter_disk() {
    flush_state();
    // Entering MSC reboots the badge. An explicit "continue without saving"
    // must discard session-only edits, just as loading the saved State would.
    storage::config=configuration::baseline; configuration::sync();
    display::brightness(storage::config.brightness);
    ble::shutdown(); usb::pad.reset(true); input::hub.clear_events();
    menu::select(-1); disk_mode=true; disk_draw_at=micros()/1000;
    set_disk_state(storage::mounted()?disk_ui::State::Ready:disk_ui::State::Unavailable);
    std::puts("HOST USB Disk preview: F3=ready F4=transferring F5=invalid FAT F6=I/O error F7=eject");
}
uint64_t number(const std::string &value) {
    size_t used=0;
    if(value.empty() || value[0]=='-') throw std::runtime_error("Invalid unsigned number: "+value);
    auto n=std::stoull(value,&used);
    if(used!=value.size()) throw std::runtime_error("Invalid unsigned number: "+value);
    return n;
}
void check_sdl(bool ok,const char *operation) {
    if(!ok) throw std::runtime_error(std::string(operation)+": "+SDL_GetError());
}
void screenshot(const std::filesystem::path &file) {
    if(!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path());
    auto *surface=SDL_CreateRGBSurfaceFrom(presented.data(),320,240,16,640,0xf800,0x07e0,0x001f,0);
    check_sdl(surface,"Screenshot surface");
    int result=SDL_SaveBMP(surface,file.string().c_str()); SDL_FreeSurface(surface);
    check_sdl(result==0,"Save screenshot");
    std::printf("HOST screenshot: %s\n",file.string().c_str());
}
std::string value(const std::string &key) {
    if(key=="view") return disk_mode?"disk":playing?"game":settings::active()?"settings":"library";
    if(key=="disk_state") return disk_states[static_cast<unsigned>(disk_state)];
    if(key=="page") return std::to_string(static_cast<int>(settings::page()));
    if(key=="selected") return std::to_string(selected);
    if(key=="count") return std::to_string(storage::count);
    if(key=="indexed") return std::to_string(storage::indexed());
    if(key=="brightness") return std::to_string(storage::config.brightness);
    if(key=="swap") return std::to_string(storage::config.swap_ab);
    if(key=="te") return std::to_string(display::te_requested());
    if(key=="dirty") return std::to_string(configuration::changed());
    if(key=="game_frames") return std::to_string(game_frames);
    if(key=="controller") return std::to_string(storage::config.controller);
    if(key=="allowed") return std::to_string(storage::config.ble.count);
    if(key=="source") return input::name(input::hub.active_remote());
    throw std::runtime_error("Unknown assertion field: "+key);
}
void action(const Action &item) {
    if(item.command=="press" || item.command=="release") {
        auto code=SDL_GetScancodeFromName(item.arg.c_str());
        if(code==SDL_SCANCODE_UNKNOWN) throw std::runtime_error("Unknown key: "+item.arg);
        keys[code]=item.command=="press";
    } else if(item.command=="shot") screenshot(script_directory/item.arg);
    else if(item.command=="source") {
        if(item.arg!="none" && item.arg!="serial" && item.arg!="ble") throw std::runtime_error("Invalid source");
        source(item.arg=="ble"?input::Source::Ble:item.arg=="serial"?input::Source::Serial:input::Source::None);
    } else if(item.command=="battery") {
        auto percent=number(item.arg); if(percent>100) throw std::runtime_error("Battery must be 0..100");
        battery_percent(unsigned(percent));
    } else if(item.command=="save-failure") {
        if(item.arg!="on" && item.arg!="off") throw std::runtime_error("save-failure requires on/off");
        fail_save=item.arg=="on";
    } else if(item.command=="disk-state") {
        if(!disk_mode) throw std::runtime_error("disk-state requires USB Disk preview");
        unsigned state=0;
        while(state<5 && item.arg!=disk_states[state]) ++state;
        if(state==5) throw std::runtime_error("Invalid disk state: "+item.arg);
        set_disk_state(static_cast<disk_ui::State>(state));
    } else if(item.command=="expect") {
        const auto separator=item.arg.find(' ');
        if(separator==std::string::npos) throw std::runtime_error("expect requires field and value");
        auto key=item.arg.substr(0,separator),expected=item.arg.substr(separator+1),actual=value(key);
        if(actual!=expected) throw std::runtime_error("Tick "+std::to_string(tick)+": "+key+" expected "+expected+", got "+actual);
    } else if(item.command=="quit") running=false;
    else throw std::runtime_error("Unknown script command: "+item.command);
}
void load_script(const std::filesystem::path &file) {
    std::ifstream in(file); if(!in) throw std::runtime_error("Cannot read script: "+file.string());
    script_directory=std::filesystem::absolute(file).parent_path();
    std::string line;
    while(std::getline(in,line)) {
        if(line.empty() || line[0]=='#') continue;
        std::istringstream row(line); std::string frame,command,arg;
        if(!(row>>frame>>command)) throw std::runtime_error("Invalid script line: "+line);
        std::getline(row>>std::ws,arg); if(!arg.empty() && arg.back()=='\r') arg.pop_back();
        auto at=number(frame);
        if(at>1000000 || (!actions.empty() && at<actions.back().tick))
            throw std::runtime_error("Script ticks must be ordered and <= 1000000");
        actions.push_back({at,command,arg});
    }
    if(actions.empty()) throw std::runtime_error("Empty preview script");
    scripted=true;
    if(!max_ticks) max_ticks=actions.back().tick+2;
    if(max_ticks<=actions.back().tick) throw std::runtime_error("--frames ends before script completion");
}
void key_event(const SDL_KeyboardEvent &event) {
    if(scripted) return;
    keys[event.keysym.scancode]=event.type==SDL_KEYDOWN;
    if(event.type!=SDL_KEYDOWN || event.repeat) return;
    if(disk_mode && event.keysym.scancode>=SDL_SCANCODE_F3 && event.keysym.scancode<=SDL_SCANCODE_F7)
        set_disk_state(static_cast<disk_ui::State>(event.keysym.scancode-SDL_SCANCODE_F3));
    if(event.keysym.scancode==SDL_SCANCODE_F2) {
        const auto old=source();
        source(old==input::Source::Ble?input::Source::Serial:old==input::Source::Serial?input::Source::None:input::Source::Ble);
    }
    if(event.keysym.scancode==SDL_SCANCODE_F12) screenshot("preview.bmp");
}
void advance() {
    ++tick;
    if(max_ticks && tick>=max_ticks) running=false;
}
void wait_until(uint32_t target) {
    if(scripted) return;
    while(running && int32_t(target-uint32_t(micros()))>0) {service(); SDL_Delay(1);}
}
void safe_service() {
    if(callback_error) return;
    try {service();}
    catch(...) {callback_error=std::current_exception(); running=false;}
}
void game_input(uint8_t &a,uint8_t &b,bool &quit) {
    safe_service(); a=input::hub.nes(storage::config.swap_ab); b=0;
    quit=!running || input::hub.home();
}
void game_line(unsigned y,const uint16_t *pixels,uint8_t mask,bool pal) {
    for(unsigned x=0;x<256;++x) screen[y*320+x+32]=presentation::token_rgb565(presentation::pixel_token(pixels[x],mask,pal));
}
presentation::FrameClock clock;
void game_frame() {
    try {
        if(callback_error) return;
        show(screen.data()); ++game_frames; advance();
        wait_until(clock.next(uint32_t(micros())));
    } catch(...) {callback_error=std::current_exception(); running=false;}
}
void game_message(const char *text) {std::printf("NES: %s\n",text);}
void run_game() {
    const uint8_t *data=nullptr; size_t bytes=0;
    if(!storage::load(selected-1,data,bytes)) return;
    nes::Cartridge cartridge; const char *error=nullptr;
    if(!nes::inspect(data,bytes,cartridge,&error)) {
        std::snprintf(storage::error,sizeof(storage::error),"%s",error?error:"Unsupported ROM"); return;
    }
    const auto now=uint32_t(micros());
    if(cartridge.timing==1) clock.reset(now,1662607,341*312*5,16);
    else if(cartridge.timing==3) clock.reset(now,1773448,341*312,3);
    else clock.reset(now,1789773,341*262,3);
    screen.fill(0); playing=true; game_frames=0;
    const nes::Host host{safe_service,game_input,game_line,game_frame,game_message};
    const bool ok=nes::infones_adapter::run(host,data,bytes,&error);
    playing=false;
    if(callback_error) std::rethrow_exception(callback_error);
    if(!ok) std::snprintf(storage::error,sizeof(storage::error),"%s",error?error:"Core failed");
    std::printf("HOST game ended: frames=%llu error=%s\n",static_cast<unsigned long long>(game_frames),error?error:"none");
}
void loop(bool open_settings,bool open_disk) {
    selected=storage::count?1:0; float position=float(selected);
    menu::Repeat repeat;
    uint32_t opened=micros()/1000;
    menu::enter(opened); menu::select(selected-1);
    input::hub.clear_events();
    if(open_settings) {selected=0; position=0; settings::open();}
    if(open_disk) enter_disk();
    while(running) {
        const uint32_t deadline=uint32_t(micros())+16667;
        service(); if(!running) break;
        const uint32_t now=micros()/1000;
        if(disk_mode) {
            input::hub.take_menu(); input::hub.take_home();
            // The device freezes the transfer frame while MSC is busy. Status
            // changes are presented once; idle/error animations run at ~30 Hz.
            if(disk_redraw || (disk_state!=disk_ui::State::Transferring && now-disk_draw_at>=33)) {
                disk_ui::draw(screen.data(),disk_state,now-disk_draw_at);
                show(screen.data()); disk_draw_at=now; disk_redraw=false;
            }
            if(disk_state==disk_ui::State::Ejected && now-disk_ejected_at>=250 && !input::hub.home()) {
                disk_mode=false;
                // Eject reboots the device: start at the first game and reset
                // session-only TE, rather than restoring a Settings selection.
                display::te_sync(true); controllers::init(); devices_init();
                storage::scan(); menu::reset_page(); selected=storage::count?1:0; position=float(selected);
                repeat.reset(); input::hub.clear_events(); menu::select(selected-1); menu::enter(now); opened=now;
            }
            advance(); wait_until(deadline); continue;
        }
        ui::service(now); configuration::sync();
        auto snapshot=input::hub.take_menu(); bool home=input::hub.take_home();
        if(settings::active()) {
            settings::update(snapshot,home,now); settings::draw(screen.data());
            show(screen.data()); settings::after_present();
            const auto exit=settings::take_exit();
            if(exit!=settings::Exit::None) {
                if(exit!=settings::Exit::ResumeLibrary) {selected=0; position=0;}
                if(exit==settings::Exit::Bootsel || exit==settings::Exit::Sleep)
                    std::puts("HOST: hardware action simulated; returning to library");
                flush_state(); repeat.reset(); menu::select(selected-1); opened=now;
                if(exit==settings::Exit::Disk) enter_disk();
            }
        } else {
            uint8_t pressed=repeat.poll(snapshot,now); const int previous=selected;
            if(pressed&input::Previous) selected=std::max(0,selected-1);
            if(pressed&input::Next) selected=std::min<int>(storage::count,selected+1);
            if(previous!=selected) storage::error[0]=0;
            if(selected>0 && !(pressed&(input::Previous|input::Next))) {
                if(pressed&input::PagePrevious) menu::previous_page();
                if(pressed&input::PageNext) menu::next_page();
            }
            if(menu::launch_requested(pressed)) {
                repeat.reset();
                if(selected==0) {settings::open(); continue;}
                input::hub.clear_events(); storage::error[0]=0; run_game(); input::hub.clear_events();
                opened=micros()/1000; menu::enter(opened);
            }
            menu::select(selected-1);
            position+=(selected-position)*0.3f;
            if(std::fabs(selected-position)<0.005f) position=selected;
            menu::draw(screen.data(),selected,position,uint32_t(micros()/1000)-opened,storage::error[0]?storage::error:nullptr);
            show(screen.data());
            if(!menu::load_current_cover()) menu::prefetch(now,snapshot.held || snapshot.pressed);
        }
        advance(); wait_until(deadline);
    }
}
}
uint64_t micros() {return scripted?tick*16667:SDL_GetTicks64()*1000-started;}
void show(const uint16_t *pixels) {
    std::copy_n(pixels,presented.size(),presented.begin());
    check_sdl(SDL_UpdateTexture(texture,nullptr,presented.data(),320*2)==0,"Update texture");
    check_sdl(SDL_RenderClear(renderer)==0,"Clear window");
    check_sdl(SDL_RenderCopy(renderer,texture,nullptr,nullptr)==0,"Render frame");
    SDL_RenderPresent(renderer);
}
void service() {
    if(!initialized) return;
    SDL_Event event;
    while(SDL_PollEvent(&event)) {
        if(event.type==SDL_QUIT) running=false;
        else if(event.type==SDL_KEYDOWN || event.type==SDL_KEYUP) key_event(event.key);
        else if(!scripted && event.type==SDL_WINDOWEVENT && event.window.event==SDL_WINDOWEVENT_FOCUS_LOST) keys.fill(false);
    }
    while(next_action<actions.size() && actions[next_action].tick<=tick) action(actions[next_action++]);
    gamepad::ControllerState remote;
    if(keys[SDL_SCANCODE_LEFT]) remote.dpad|=gamepad::West;
    if(keys[SDL_SCANCODE_RIGHT]) remote.dpad|=gamepad::East;
    if(keys[SDL_SCANCODE_UP]) remote.dpad|=gamepad::North;
    if(keys[SDL_SCANCODE_DOWN]) remote.dpad|=gamepad::South;
    if(keys[SDL_SCANCODE_Z] || (!playing && keys[SDL_SCANCODE_RETURN])) remote.keys|=gamepad::KeyA;
    if(keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_ESCAPE]) remote.keys|=gamepad::KeyB;
    if(keys[SDL_SCANCODE_RETURN] && playing) remote.keys|=gamepad::KeyStart;
    if(keys[SDL_SCANCODE_RSHIFT]) remote.keys|=gamepad::KeySelect;
    if(keys[SDL_SCANCODE_HOME]) remote.keys|=gamepad::KeyHome;
    if(!disk_mode) devices_poll(remote);
    const uint8_t local=(keys[SDL_SCANCODE_Q]?input::LocalA:0)|(keys[SDL_SCANCODE_W]?input::LocalB:0)|
        (keys[SDL_SCANCODE_E]?input::LocalC:0)|(keys[SDL_SCANCODE_I]?input::LocalUp:0)|
        (keys[SDL_SCANCODE_K]?input::LocalDown:0)|(keys[SDL_SCANCODE_H]?input::LocalHome:0);
    input::hub.local(local);
}
int main(int argc,char **argv) {
    int result=0;
    try {
        std::filesystem::path images,script;
        bool headless=false,settings=false,disk=false;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            auto argument=[&]() -> std::string {if(++i>=argc) throw std::runtime_error("Missing value for "+arg); return argv[i];};
            if(arg=="--images") images=argument();
            else if(arg=="--script") script=argument();
            else if(arg=="--frames") {max_ticks=number(argument()); if(!max_ticks || max_ticks>1000000) throw std::runtime_error("--frames must be 1..1000000");}
            else if(arg=="--headless") headless=true;
            else if(arg=="--settings") settings=true;
            else if(arg=="--disk") disk=true;
            else if(arg=="--help") {
                std::puts("tufty_preview --images DIR [--settings | --disk] [--headless] [--script FILE] [--frames N]\n"
                    "Arrows: navigate / NES D-pad; Z/Enter: confirm; X/Escape: back\n"
                    "Game: Z=A X=B Enter=Start Right Shift=Select Home=exit\n"
                    "Local fallback: Q/W/E=A/B/C I/K=Up/Down H=Home\n"
                    "F2: BLE / serial / disconnected; F12: preview.bmp\n"
                    "Disk preview: F3=ready F4=transferring F5=invalid FAT F6=I/O error F7=eject\n"
                    "Hardware status/radio are simulated. No audio, disk export, sleep or BOOTSEL."); return 0;
            } else throw std::runtime_error("Unknown argument: "+arg);
        }
        if(images.empty()) throw std::runtime_error("--images is required; use tools/preview.py to prepare resources");
        if(settings && disk) throw std::runtime_error("Choose either --settings or --disk");
        SDL_SetMainReady();
        if(headless) SDL_setenv("SDL_VIDEODRIVER","dummy",1);
        check_sdl(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)==0,"Init SDL2");
        started=SDL_GetTicks64()*1000;
        if(!script.empty()) load_script(script);
        if(headless && !scripted && !max_ticks) throw std::runtime_error("--headless requires --frames or --script");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"0");
        window=SDL_CreateWindow("Tufty NES - Host Preview (simulated hardware)",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,960,720,
            SDL_WINDOW_RESIZABLE|(headless?SDL_WINDOW_HIDDEN:0)); check_sdl(window,"Create window");
        SDL_SetWindowMinimumSize(window,320,240);
        renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE); check_sdl(renderer,"Create renderer");
        check_sdl(SDL_RenderSetLogicalSize(renderer,320,240)==0,"Set logical size");
        SDL_RenderSetIntegerScale(renderer,SDL_TRUE);
        texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,320,240); check_sdl(texture,"Create texture");
        mount_flash(images); storage::init(); configuration::init(); controllers::init(); devices_init();
        if(!storage::mounted() && !disk) throw std::runtime_error(storage::error);
        storage::scan(); menu::load_art(); initialized=true;
        std::printf("HOST library: %zu games, indexed=%d, writable_state=%d, brightness=%u\n",storage::count,storage::indexed(),storage::state_ready(),storage::config.brightness);
        loop(settings,disk);
        if(next_action!=actions.size()) throw std::runtime_error("Preview ended before script completed");
        if(configuration::changed()) {
            const auto saved=configuration::save();
            if(saved!=configuration::SaveResult::Saved) throw std::runtime_error("Host settings could not be saved on exit");
        }
        flush_state();
    } catch(const std::exception &e) {std::cerr<<"Host preview: "<<e.what()<<'\n'; result=1;}
    unmount_flash();
    if(texture) SDL_DestroyTexture(texture);
    if(renderer) SDL_DestroyRenderer(renderer);
    if(window) SDL_DestroyWindow(window);
    SDL_Quit(); return result;
}
}
int main(int argc,char **argv) {return preview::main(argc,argv);}
