#include "settings.hpp"
#include "library.hpp"
#include "configuration.hpp"
#include "controllers.hpp"
#include "ble.hpp"
#include "menu.hpp"
#include "ui.hpp"
#include "battery.hpp"
#include "background.hpp"
#include "side_background.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static bool writable=true,save_ok=true;
static bool reset_ok=true;
static unsigned resets=0;
namespace library {
bool edited=false,save_ok=true;
unsigned saves=0,discards=0;
bool pending() {return edited;}
bool save() {++saves; if(save_ok) edited=false; return save_ok;}
void discard() {++discards; edited=false;}
bool custom_order() {return false;}
bool move_to_front(size_t,storage::Progress) {return true;}
bool reset_order(storage::Progress) {return true;}
}
static unsigned icon_fixture=0;
static unsigned saves=0,reads=0,searches=0,stops=0,connects=0,bright=80;
static bool requested_te=true;
static display::Stats panel;
static ble::Status radio{ble::State::Scanning,true,false,false,{},0};
static bluetooth::Candidate found[16];
static size_t found_count=0;
static gamepad::Receiver ble_pad;
struct BackgroundFile {std::string path; uint32_t bytes=320*240*2; uint16_t color=0; bool fail=false,ramp=false;};
static std::vector<BackgroundFile> backgrounds;
static bool background_scan_ok=true;
static void dispatch_input(uint32_t now) {
    const auto pad=input::hub.gamepad();
    const auto keys=input::menu(pad);
    input::hub.acknowledge(pad);
    settings::update(keys,pad.pressed&gamepad::KeyHome,now);
}
namespace battery {extern Reading test_value;}
namespace storage {
Config config;
Entry table[128]{}; Entry *entries=table; size_t count=0; char error[96]{};
bool state_ready() {return writable;}
FileStatus file_size(const char *,size_t &bytes) {bytes=0; return FileStatus::Missing;}
bool write_state(const char *,const void *,size_t,Progress) {return save_ok;}
bool save() {++saves; return save_ok;}
bool reset_state() {++resets; return reset_ok;}
bool visit_files(const char *directory,Visitor visitor,void *context) {
    const auto prefix=std::string(directory)+"/";
    for(const auto &file:backgrounds) if(file.path.rfind(prefix,0)==0)
        visitor(file.path.c_str()+prefix.size(),file.bytes,context);
    return background_scan_ok;
}
bool read_file(const char *path,void *out,size_t capacity,size_t &actual) {
    ++reads; actual=0;
    for(const auto &file:backgrounds) if(file.path==path) {
        if(file.bytes>capacity) return false;
        actual=file.bytes;
        std::fill_n(static_cast<uint16_t *>(out),actual/2,file.color);
        if(file.ramp) for(size_t i=0;i<actual/2;++i) static_cast<uint16_t *>(out)[i]=uint16_t(file.color+i);
        if(file.fail) {actual/=2; return false;}
        return true;
    }
    if(std::strncmp(path,"/rom/settings/",14)) return false;
    if(icon_fixture==5) {
        const char *root=std::getenv("TUFTY_SETTINGS_ASSETS");
        if(!root) return false;
        std::ifstream file(std::string(root)+path,std::ios::binary|std::ios::ate);
        if(!file || file.tellg()<0 || size_t(file.tellg())>capacity) return false;
        actual=size_t(file.tellg()); file.seekg(0);
        return bool(file.read(static_cast<char *>(out),actual));
    }
    if(!icon_fixture) return false;
    auto *pixels=static_cast<uint8_t *>(out);
    for(size_t i=0;i<capacity;i+=2) {pixels[i]=0x1f; pixels[i+1]=0xf8;}
    pixels[2]=0xe0; pixels[3]=0x07; // One opaque green pixel after transparent magenta.
    actual=icon_fixture==2?capacity-1:icon_fixture==4?capacity+1:capacity;
    return icon_fixture!=3;
}
uint32_t generation() {return 1;}
bool indexed() {return true;}
bool has_cover(size_t i) {assert(i<count); return false;}
const char *title(size_t i) {assert(i<count); return "Game";}
const char *description(size_t i) {assert(i<count); return "Description";}
}
namespace usb {tgp::Receiver pad; bool dirty=false,save_requested=false;}
namespace display {
void brightness(uint8_t n) {bright=n;}
void te_sync(bool on) {requested_te=on;}
bool te_requested() {return requested_te;}
Stats stats() {return panel;}
}
namespace power {void activity() {}}
namespace ble {
Status status() {return radio;}
void configure(const bluetooth::Configuration &cfg,bool) {radio.enabled=cfg.enabled;}
gamepad::Receiver &input_receiver() {return ble_pad;}
bool discover(uint32_t) {++searches; radio.discovering=true; return radio.enabled;}
void stop_discovery() {++stops; radio.discovering=false;}
size_t candidates(Candidate *out,size_t capacity) {
    if(!out) return found_count;
    size_t n=std::min(capacity,found_count); std::copy_n(found,n,out); return n;
}
size_t candidate_count() {return found_count;}
bool connect(const bluetooth::Peer &peer) {++connects; radio.peer=peer; radio.state=State::Connecting; radio.discovering=false; return true;}
}
void tufty_service() {}
static uint32_t now=0;
static std::vector<uint16_t> frame(320*240+32,0x1234);
static void press(uint8_t key) {settings::update({key,key,0,input::hub.epoch()},false,++now);}
static void back() {press(input::Back);}
static void home() {settings::update({0,0,0,input::hub.epoch()},true,++now);}
static void focus(unsigned wanted) {
    for(int i=0;i<20 && settings::focus()!=wanted;++i)
        press(settings::focus()<wanted?input::PageNext:input::PagePrevious);
    assert(settings::focus()==wanted);
}
static void enter(unsigned row,settings::Page expected) {focus(row); press(input::Confirm); assert(settings::page()==expected);}
static void open_id(unsigned row=4) {
    focus(row); press(input::Confirm);
    assert(settings::page()==settings::Page::Bluetooth && settings::editing_id());
}
static void root() {
    settings::open(); assert(settings::page()==settings::Page::Root);
}
static void present() {
    const auto before=reads; settings::draw(frame.data()); assert(reads==before);
    for(size_t i=320*240;i<frame.size();++i) assert(frame[i]==0x1234);
    settings::after_present();
}
static void snapshot(const char *name,bool library=false) {
    const char *directory=std::getenv("TUFTY_SETTINGS_PREVIEW"); if(!directory) return;
    if(library) menu::draw(frame.data(),0,0,300); else settings::draw(frame.data());
    std::ofstream out(std::string(directory)+"/"+name+".ppm",std::ios::binary);
    out<<"P6\n320 240\n255\n";
    for(size_t i=0;i<320*240;++i) {uint16_t v=frame[i]; char c[]={char((v>>11)*255/31),char(((v>>5)&63)*255/63),char((v&31)*255/31)}; out.write(c,3);}
}
int main() {
    // Transparent key, little-endian decode, clipping on every edge, cached
    // missing/corrupt results, and reload invalidation use the actual renderer.
    std::vector<uint16_t> guarded(320*240+32,0x1234);
    auto *pixels=guarded.data()+16;
    ui::load_icons();
    const auto missing_reads=reads;
    assert(!ui::icon(pixels,ui::Icon::Header,0,0));
    assert(!ui::icon(pixels,ui::Icon::Header,0,0) && reads==missing_reads);
    icon_fixture=1; ui::load_icons();
    assert(ui::icon(pixels,ui::Icon::Header,0,0));
    assert(pixels[0]==0x1234 && pixels[1]==0x07e0);
    assert(ui::icon(pixels,ui::Icon::Settings,-1,0) && pixels[0]==0x07e0);
    assert(ui::icon(pixels,ui::Icon::Settings,319,239));
    assert(ui::icon(pixels,ui::Icon::Control,-19,-19));
    assert(ui::icon(pixels,ui::Icon::Header,400,300));
    assert(!ui::icon(pixels,ui::Icon::Count,0,0));
    for(unsigned i=0;i<16;++i) assert(guarded[i]==0x1234 && guarded[320*240+16+i]==0x1234);
    for(icon_fixture=2;icon_fixture<=4;++icon_fixture) {
        ui::load_icons(); const auto failed_reads=reads;
        assert(!ui::icon(pixels,ui::Icon::Header,0,0));
        assert(!ui::icon(pixels,ui::Icon::Settings,0,0) && reads==failed_reads);
    }
    icon_fixture=std::getenv("TUFTY_SETTINGS_ASSETS")?5:0;
    ui::load_icons();
    // Center visible ink, not the 19-row padded bitmap. Ascenders and
    // descenders must align with the same row/cursor center.
    for(const char *label:{"Control","Brightness","TE sync","100%"}) {
        std::fill(pixels,pixels+320*240,0);
        ui::text_centered(pixels,label,20,40,24,ui::white,200);
        int first=240,last=-1;
        for(int y=0;y<240;++y) for(int x=0;x<320;++x) if(pixels[y*320+x]) {
            first=std::min(first,y); last=std::max(last,y);
        }
        assert(first>=40 && last<64 && std::abs(first+last-103)<=1);
    }
    std::fill(pixels,pixels+320*240,0); ui::status_icons(pixels);
    assert(pixels[ui::status_center_y*320+305]!=0 && pixels[24*320+305]==0);
    // Settings uses a contrasting palette; the library keeps its gradient.
    battery::test_value.valid=true; battery::test_value.percent=72;
    ui::status_icons(pixels); const auto library_battery=pixels[10*320+270];
    ui::status_icons(pixels,ui::BatteryStyle::Settings);
    assert(pixels[10*320+270]==ui::rgb(36,80,57) && pixels[10*320+270]!=library_battery);
    battery::test_value.percent=10; ui::status_icons(pixels,ui::BatteryStyle::Settings);
    assert(pixels[10*320+270]==ui::rgb(158,43,31));
    battery::test_value.valid=false; ui::status_icons(pixels,ui::BatteryStyle::Settings);
    assert(pixels[10*320+270]==ui::rgb(81,79,71));
    battery::test_value.valid=true; battery::test_value.percent=72;
    configuration::init();
    // Logical fields, including MAC address type, define persistence; unused slots do not.
    auto copy=storage::config; copy.ble.peers[3].random=true;
    assert(configuration::equal(copy,storage::config));
    bluetooth::Peer peer; peer.address[5]=1;
    storage::config.ble.add(peer); configuration::sync(); assert(usb::dirty);
    storage::config.ble={}; configuration::sync(); assert(!usb::dirty);
    configuration::brightness(75); assert(bright==75 && usb::dirty);
    configuration::brightness(80); assert(!usb::dirty);
    // Synthetic card does not access ROM metadata or start prefetch, even in an empty library.
    menu::select(-1); menu::draw(frame.data(),0,0,300);
    snapshot("library-settings",true);
    assert(!menu::load_current_cover() && !menu::prefetch(1000,false));
    root(); snapshot("root");
    settings::draw(frame.data()); assert(frame[0]==ui::rgb(184,176,160));
    for(int y=32;y<208;++y) assert(frame[y*320+319]==ui::rgb(229,218,194));
    assert(!settings::title_selected());
    assert(frame[198*320+150]==ui::rgb(16,20,24)); // Panel fills unused space.
    assert(frame[97*320+150]==ui::rgb(116,109,97)); // Option divider.
    // Right enters a child page. Its held state must not also adjust that page.
    focus(0); input::hub.local(input::LocalC);
    dispatch_input(++now);
    assert(settings::page()==settings::Page::Display);
    dispatch_input(now+=600);
    assert(bright==80);
    input::hub.local(0); dispatch_input(++now);
    snapshot("display");
    settings::draw(frame.data()); assert(frame[182*320+285]==ui::rgb(70,220,100));
    focus(0); press(input::Previous); // Left changes panes, never brightness.
    assert(bright==80 && settings::page()==settings::Page::Root);
    press(input::Next); assert(settings::page()==settings::Page::Display);
    settings::draw(frame.data()); const auto before_popup=frame;
    press(input::Confirm); snapshot("brightness");
    settings::draw(frame.data());
    assert(std::equal(frame.begin(),frame.begin()+32*320,before_popup.begin()));
    assert(std::equal(frame.begin()+208*320,frame.begin()+240*320,before_popup.begin()+208*320));
    press(input::Previous); assert(bright==75);
    back(); assert(settings::page()==settings::Page::Display);
    back(); assert(saves==0); back(); assert(saves==0 && settings::transitioning());
    present(); assert(saves==1 && settings::take_exit()==settings::Exit::Library && !usb::dirty);
    root(); enter(0,settings::Page::Display); focus(4);
    press(input::Next); assert(requested_te); // Right does not toggle TE.
    press(input::Previous); assert(requested_te && settings::page()==settings::Page::Root);
    press(input::Next); assert(settings::focus()==4);
    press(input::Confirm);
    assert(!requested_te && !configuration::changed());
    press(input::Next); assert(!requested_te);
    settings::draw(frame.data()); assert(frame[182*320+285]==ui::rgb(100,100,100));
    back(); back(); present();
    assert(saves==1 && settings::take_exit()==settings::Exit::Library);
    // Failed writes preserve runtime values and baseline; cancellation never silently saves.
    root(); configuration::brightness(70); save_ok=false; back(); present(); snapshot("save-error");
    assert(saves==2 && settings::active() && configuration::changed());
    press(input::PageNext); press(input::PageNext); press(input::Confirm);
    assert(!settings::transitioning());
    back(); present(); assert(saves==3);
    press(input::Confirm); save_ok=true; present();
    assert(saves==4 && settings::take_exit()==settings::Exit::Library && !configuration::changed());
    // Explicit session-only exit retains changes, and later exits retry.
    root(); configuration::brightness(65); save_ok=false; back(); present();
    press(input::PageNext); press(input::Confirm); present();
    assert(settings::take_exit()==settings::Exit::Library && configuration::changed());
    root(); save_ok=true; back(); present(); assert(settings::take_exit()==settings::Exit::Library);
    writable=false; configuration::init(); root(); back(); present();
    assert(std::strstr(settings::notice(),"unavailable"));
    press(input::PageNext); press(input::Confirm); present(); assert(settings::take_exit()==settings::Exit::Library);
    writable=true; configuration::init();
    root(); enter(1,settings::Page::Control); snapshot("control");
    focus(0); press(input::Next); assert(settings::page()==settings::Page::Bluetooth);
    snapshot("bluetooth-on");
    enter(3,settings::Page::Mode); snapshot("mode");
    settings::draw(frame.data());
    assert(frame[86*320+285]==ui::rgb(70,220,100) && frame[110*320+285]==ui::rgb(100,100,100));
    focus(1); press(input::Previous);
    assert(settings::title_selected() && settings::page()==settings::Page::Mode);
    press(input::Confirm);
    assert(settings::page()==settings::Page::Bluetooth && settings::focus()==3);
    open_id(); snapshot("id");
    // External same-field edit must not be overwritten by an old draft.
    storage::config.controller=42; press(input::Confirm);
    assert(settings::editing_id() && std::strstr(settings::notice(),"externally"));
    back(); open_id();
    press(input::PagePrevious); press(input::Confirm); assert(storage::config.controller==10042);
    assert(!settings::editing_id() && settings::focus()==4);
    open_id();
    for(int i=0;i<9;++i) press(input::PagePrevious);
    press(input::Confirm); assert(settings::editing_id() && storage::config.controller==10042);
    back(); enter(1,settings::Page::Search); assert(searches==1);
    found_count=2; found[0].peer=peer; found[1].peer=peer; found[1].peer.random=true;
    std::strcpy(found[0].name,"Long gamepad name");
    radio.discovering=false; settings::update({},false,now+=10000); snapshot("search");
    enter(1,settings::Page::Device); snapshot("device"); assert(stops==0);
    // Snapshot identity is stable even if backend order changes.
    found[1].peer.address[5]=99;
    focus(1); press(input::Confirm);
    assert(storage::config.ble.count==1 && storage::config.ble.peers[0].random && storage::config.ble.peers[0].address[5]==1);
    focus(0); press(input::Confirm); assert(connects==1 && radio.peer.random);
    back(); assert(searches==1 && stops==0 && settings::focus()==1);
    back(); assert(stops==1);
    enter(2,settings::Page::Allowed); enter(0,settings::Page::AllowedDevice);
    focus(1); press(input::Confirm); assert(storage::config.ble.preferred==0);
    focus(2); press(input::Confirm); press(input::PagePrevious); press(input::Confirm);
    assert(settings::page()==settings::Page::Allowed && !storage::config.ble.count);
    // Configuration-only pages remain available when Bluetooth is disabled.
    storage::config.ble_enabled=false; settings::update({},false,++now);
    assert(settings::page()==settings::Page::Allowed);
    back(); assert(settings::page()==settings::Page::Bluetooth); snapshot("bluetooth-off");
    enter(1,settings::Page::Allowed); // Search is hidden; row 1 is the allow list.
    back(); enter(2,settings::Page::Mode);
    back(); open_id(3); back();
    // With Connect hidden, row zero edits preference without touching the radio.
    assert(controllers::whitelist_add(peer));
    enter(1,settings::Page::Allowed); enter(0,settings::Page::AllowedDevice); snapshot("device-off");
    const auto connections_before=connects;
    focus(0); press(input::Confirm);
    assert(connects==connections_before && storage::config.ble.preferred==0);
    back(); back(); back(); assert(settings::page()==settings::Page::Control);
    // Non-preset sleep values are unchanged on entry, then explicitly adjusted.
    back(); storage::config.menu_sleep_seconds=77; enter(2,settings::Page::Power); snapshot("power");
    assert(storage::config.menu_sleep_seconds==77); focus(0); press(input::Next);
    assert(storage::config.menu_sleep_seconds==77); snapshot("sleep-time");
    press(input::Next); assert(storage::config.menu_sleep_seconds==180);
    press(input::Previous); assert(storage::config.menu_sleep_seconds==60);
    back();
    back(); enter(3,settings::Page::System); snapshot("system"); back(); enter(4,settings::Page::About); snapshot("about");
    // System exits also save after presentation and wait for release.
    input::hub.local(input::LocalB); settings::request_exit(settings::Exit::Bootsel);
    present(); assert(settings::take_exit()==settings::Exit::None);
    input::hub.local(0); present(); assert(settings::take_exit()==settings::Exit::Bootsel);
    assert(!configuration::changed());
    // Editing back to the saved value never writes. Endpoints remain reachable.
    root(); enter(0,settings::Page::Display); focus(0); press(input::Confirm);
    const auto saved_brightness=storage::config.brightness;
    const auto count_before=saves;
    for(int i=0;i<25;++i) press(input::Previous);
    assert(bright==1);
    for(int i=0;i<25;++i) press(input::Next);
    assert(bright==100);
    configuration::brightness(saved_brightness); back(); back(); back(); present();
    assert(saves==count_before && settings::take_exit()==settings::Exit::Library);
    // Console save stays on a settings subpage, and late Console edits are
    // saved again before a held-button mode transition can complete.
    root(); enter(0,settings::Page::Display); configuration::brightness(50);
    settings::request_save(); present();
    assert(settings::active() && settings::page()==settings::Page::Display && !settings::transitioning());
    input::hub.local(input::LocalB); settings::request_exit(settings::Exit::Disk); present();
    configuration::brightness(55); present(); assert(settings::take_exit()==settings::Exit::None);
    const auto before_late=saves;
    input::hub.local(0); present();
    assert(saves==before_late+1 && settings::take_exit()==settings::Exit::Disk && !configuration::changed());
    // A handover cannot carry an old edge into the next page.
    root(); focus(0); input::hub.clear_events();
    press(input::Confirm); assert(settings::page()==settings::Page::Root);
    press(input::Confirm); assert(settings::page()==settings::Page::Display);
    // Opposite directions cancel and no confirmation rides a navigation event.
    focus(0); const auto level=bright;
    press(input::Previous|input::Next); assert(bright==level);
    back(); focus(0); press(input::PageNext|input::Confirm);
    assert(settings::page()==settings::Page::Root && settings::focus()==1);
    // Full allow list rejects a fifth peer, preserving prior identities.
    storage::config.ble={};
    for(unsigned i=0;i<4;++i) {peer.address[5]=i; assert(controllers::whitelist_add(peer));}
    const auto full=storage::config.ble; peer.address[5]=99;
    assert(!controllers::whitelist_add(peer,true));
    assert(storage::config.ble.count==4 && storage::config.ble.preferred==full.preferred);
    for(unsigned i=0;i<4;++i) assert(storage::config.ble.peers[i]==full.peers[i]);
    // A console BLE-off during a failed save must retain the failure dialog.
    storage::config.ble_enabled=true; root(); enter(1,settings::Page::Control);
    enter(0,settings::Page::Bluetooth); save_ok=false;
    settings::request_exit(settings::Exit::Disk); present();
    storage::config.ble_enabled=false;
    settings::update({0,0,0,input::hub.epoch()},false,++now);
    assert(settings::page()==settings::Page::Bluetooth && settings::transitioning());
    press(input::PageNext); press(input::Confirm); present();
    assert(settings::take_exit()==settings::Exit::Disk && configuration::changed());
    // The disabled Bluetooth entry is still navigable; only its secondary
    // first row toggles it. Hidden actions cannot consume a stale edge.
    save_ok=true; root(); enter(1,settings::Page::Control);
    enter(0,settings::Page::Bluetooth); assert(!storage::config.ble_enabled);
    focus(0); press(input::Confirm); assert(storage::config.ble_enabled);
    settings::update({},false,++now);
    radio.state=ble::State::Scanning;
    enter(1,settings::Page::Search);
    storage::config.ble_enabled=false;
    const auto search_before=searches;
    press(input::Confirm);
    assert(settings::page()==settings::Page::Bluetooth && settings::focus()==0);
    assert(!storage::config.ble_enabled && searches==search_before);
    enter(1,settings::Page::Allowed); enter(0,settings::Page::AllowedDevice);
    storage::config.ble_enabled=true;
    const auto connect_before=connects;
    press(input::Confirm);
    assert(connects==connect_before && settings::focus()==0);
    press(input::Confirm); assert(connects==connect_before+1);
    back(); back(); back(); back();
    assert(settings::page()==settings::Page::Root && settings::focus()==1);
    const auto before_draw=reads;
    settings::draw(frame.data());
    assert(reads==before_draw && settings::page()==settings::Page::Root && settings::focus()==1);
    // A full discovery list still reaches its final action with five visible
    // rows, without overwriting the footer or losing the scroll indicator.
    radio.state=ble::State::Scanning;
    enter(1,settings::Page::Control); enter(0,settings::Page::Bluetooth);
    enter(1,settings::Page::Search);
    found_count=16;
    for(unsigned i=0;i<16;++i) {
        found[i]={}; found[i].peer.address[5]=uint8_t(i);
        std::strcpy(found[i].name,"Test gamepad");
    }
    radio.discovering=false; settings::update({},false,now+=10000);
    // Reverse direction within the visible five rows without scrolling back.
    focus(6); present();
    assert(frame[(74+4*24+6)*320+15]==ui::cyan);
    const std::vector<uint16_t> window_before=frame;
    press(input::PagePrevious); present();
    assert(settings::focus()==5 && frame[(74+3*24+6)*320+15]==ui::cyan);
    for(int y=74;y<194;++y) assert(frame[y*320+309]==window_before[y*320+309]);
    focus(2); present();
    assert(frame[80*320+15]==ui::cyan);
    for(int y=74;y<194;++y) assert(frame[y*320+309]==window_before[y*320+309]);
    press(input::PagePrevious); present();
    assert(settings::focus()==1 && frame[80*320+15]==ui::cyan);
    assert(frame[81*320+309]!=window_before[81*320+309]);
    // Opening a device and returning preserves this list's window.
    focus(6); press(input::PagePrevious);
    press(input::Confirm); assert(settings::page()==settings::Page::Device);
    back(); present();
    assert(settings::page()==settings::Page::Search && settings::focus()==5);
    assert(frame[(74+3*24+6)*320+15]==ui::cyan);
    focus(0);
    snapshot("search-long-top"); focus(16); snapshot("search-long-bottom");
    present();
    assert(frame[193*320+309]==ui::rgb(229,218,194));
    assert(frame[210*320+150]==ui::rgb(171,163,146));
    for(int y=32;y<208;++y) assert(frame[y*320+319]==ui::rgb(229,218,194));
    // Left reaches the title from any row, without exiting on the same press
    // or borrowing Confirm. Opposite directions and held Left do not return.
    press(input::Previous|input::Next); assert(!settings::title_selected());
    press(input::Previous); // Clear the axis conflict before a fresh edge.
    press(input::Previous|input::Confirm);
    assert(settings::title_selected() && settings::page()==settings::Page::Search && settings::focus()==16);
    settings::update({input::Previous,0,0,input::hub.epoch()},false,now+=600);
    assert(settings::title_selected() && settings::page()==settings::Page::Search);
    press(input::PageNext); assert(!settings::title_selected() && settings::focus()==0);
    // Every full-screen list has a reachable title return, including long
    // lists and an in-progress search. Only the fixed back icon is highlighted.
    focus(0); press(input::PagePrevious);
    assert(settings::title_selected()); snapshot("search-return");
    settings::draw(frame.data()); assert(frame[42*320+10]==ui::rgb(43,57,72));
    assert(frame[35*320+100]==ui::rgb(16,24,33));
    press(input::PagePrevious|input::Confirm);
    assert(settings::page()==settings::Page::Search && settings::title_selected());
    press(input::PageNext); assert(!settings::title_selected() && settings::focus()==0);
    press(input::PagePrevious); press(input::Confirm);
    assert(settings::page()==settings::Page::Bluetooth && !settings::title_selected());
    enter(1,settings::Page::Search);
    const auto active_search_stops=stops;
    press(input::Previous); assert(settings::title_selected() && stops==active_search_stops);
    press(input::Confirm);
    assert(settings::page()==settings::Page::Bluetooth && stops==active_search_stops+1);
    enter(1,settings::Page::Search); press(input::PagePrevious);
    radio.discovering=false; settings::update({},false,now+=10000);
    assert(settings::title_selected()); press(input::Confirm);
    assert(settings::page()==settings::Page::Bluetooth);
    // ID opens directly into a modal editor: text is never a selectable row,
    // Left stops at the first digit and Confirm closes from any digit.
    open_id();
    const auto original_id=storage::config.controller;
    const auto id_focus=settings::focus();
    press(input::Previous); assert(settings::editing_id() && settings::focus()==id_focus && !settings::title_selected());
    for(int i=0;i<4;++i) press(input::Next);
    press(input::Confirm);
    assert(!settings::editing_id() && settings::page()==settings::Page::Bluetooth && settings::focus()==id_focus);
    assert(storage::config.controller==original_id);
    open_id();
    for(int i=0;i<9;++i) press(input::PagePrevious);
    press(input::Confirm); assert(settings::editing_id());
    snapshot("id-invalid"); back(); assert(!settings::editing_id());
    assert(settings::page()==settings::Page::Bluetooth && storage::config.controller==original_id);
    storage::config.controller=1; open_id();
    for(int i=0;i<4;++i) press(input::Next);
    press(input::PageNext); press(input::Confirm);
    assert(settings::editing_id() && storage::config.controller==1); // Zero is invalid.
    press(input::PagePrevious); press(input::Confirm); assert(!settings::editing_id());
    storage::config.controller=65535; open_id(); press(input::Confirm);
    assert(!settings::editing_id() && storage::config.controller==65535);
    storage::config.controller=original_id;
    focus(0); press(input::PagePrevious); press(input::Confirm);
    assert(settings::page()==settings::Page::Control);
    // Home exits from either pane, a deep page, an editor, or any popup.
    home(); present(); assert(settings::take_exit()==settings::Exit::Library);
    root(); home(); present(); assert(settings::take_exit()==settings::Exit::Library);
    root(); enter(0,settings::Page::Display); focus(0); press(input::Confirm);
    press(input::Previous); const auto home_level=storage::config.brightness;
    home(); present(); assert(settings::take_exit()==settings::Exit::Library);
    assert(storage::config.brightness==home_level && !configuration::changed());
    root(); enter(1,settings::Page::Control); enter(0,settings::Page::Bluetooth);
    open_id(); press(input::PagePrevious);
    home(); present(); assert(settings::take_exit()==settings::Exit::Library);
    assert(storage::config.controller==original_id);
    root(); enter(1,settings::Page::Control); enter(0,settings::Page::Bluetooth);
    enter(1,settings::Page::Search);
    const auto home_search_stops=stops;
    home(); present(); assert(settings::take_exit()==settings::Exit::Library && stops==home_search_stops+1);
    root(); enter(3,settings::Page::System); focus(1); press(input::Confirm);
    home(); present(); assert(settings::take_exit()==settings::Exit::Library);
    // Home can replace a pending system action, but cannot bypass a failed
    // save or the neutral barrier, including while Save/Release is pending.
    root(); configuration::brightness(60); save_ok=false;
    settings::request_exit(settings::Exit::Disk); present();
    home(); present(); assert(settings::take_exit()==settings::Exit::None && settings::active());
    press(input::PageNext); press(input::Confirm); present();
    assert(settings::take_exit()==settings::Exit::Library && configuration::changed());
    root(); save_ok=true; settings::request_exit(settings::Exit::Bootsel);
    input::hub.local(input::LocalHome);
    dispatch_input(++now);
    present(); assert(settings::take_exit()==settings::Exit::None && !configuration::changed());
    dispatch_input(now+=600);
    present(); assert(settings::take_exit()==settings::Exit::None);
    input::hub.local(0); present(); assert(settings::take_exit()==settings::Exit::Library);
    root(); input::hub.local(input::LocalB);
    settings::request_exit(settings::Exit::Disk); present(); home();
    input::hub.local(0); present(); assert(settings::take_exit()==settings::Exit::Library);
    // Console can edit the allowed list between presentation and input.
    // A row's old Confirm must never open the peer that shifted into its slot.
    storage::config.ble={};
    bluetooth::Peer first_peer{},second_peer{};
    first_peer.address[5]=1; second_peer.address[5]=2;
    assert(controllers::whitelist_add(first_peer) && controllers::whitelist_add(second_peer));
    root(); enter(1,settings::Page::Control); enter(0,settings::Page::Bluetooth);
    enter(2,settings::Page::Allowed); focus(1);
    controllers::whitelist_remove(first_peer); press(input::Confirm);
    assert(settings::page()==settings::Page::Allowed && settings::focus()==0);
    press(input::Confirm); assert(settings::page()==settings::Page::AllowedDevice);
    focus(0); press(input::Confirm); assert(radio.peer==second_peer);
    // Removing the displayed peer externally closes its stale action page.
    const auto connects_before_removal=connects;
    controllers::whitelist_remove(second_peer); press(input::Confirm);
    assert(settings::page()==settings::Page::Allowed && connects==connects_before_removal);
    // A pending Clear confirmation must not include newly added peers.
    press(input::Confirm); press(input::PagePrevious);
    assert(controllers::whitelist_add(first_peer)); press(input::Confirm);
    assert(settings::page()==settings::Page::Allowed && storage::config.ble.count==1);
    // Custom Console values above the final preset cannot decrease on Right.
    root(); storage::config.menu_sleep_seconds=3600;
    enter(2,settings::Page::Power); focus(0); press(input::Confirm);
    press(input::Next); assert(storage::config.menu_sleep_seconds==3600);
    press(input::Previous); assert(storage::config.menu_sleep_seconds==1800);
    // Home during an explicit save still leaves the discovery flow. Keeping
    // the save gate must not leave the radio waiting for a choice off-screen.
    back(); home(); present(); assert(settings::take_exit()==settings::Exit::Library);
    root(); radio.state=ble::State::Scanning;
    enter(1,settings::Page::Control); enter(0,settings::Page::Bluetooth);
    enter(1,settings::Page::Search);
    const auto save_search_stops=stops;
    settings::request_save();
    home(); present();
    assert(settings::take_exit()==settings::Exit::Library);
    assert(stops==save_search_stops+1 && !radio.discovering);
    // Order-only edits join the same exit gate, without rewriting configuration.
    writable=save_ok=library::save_ok=true; configuration::init();
    const auto config_saves=saves;
    for(auto destination:{settings::Exit::Library,settings::Exit::Disk,settings::Exit::Bootsel,
                          settings::Exit::Sleep,settings::Exit::Rebuild}) {
        root(); library::edited=true;
        const auto order_saves=library::saves;
        settings::request_exit(destination); present();
        assert(settings::take_exit()==destination && !library::pending());
        assert(library::saves==order_saves+1 && saves==config_saves);
    }
    root(); library::edited=true; library::save_ok=false;
    home(); present();
    assert(settings::active() && library::pending() && std::strstr(settings::notice(),"failed"));
    press(input::PageNext); press(input::PageNext); press(input::Confirm); // Cancel exit.
    assert(!settings::transitioning() && library::pending());
    home(); present(); library::save_ok=true;
    press(input::Confirm); present(); // Retry save.
    assert(settings::take_exit()==settings::Exit::Library && !library::pending());
    root(); library::edited=true; library::save_ok=false;
    const auto discarded=library::discards;
    home(); present(); press(input::PageNext); press(input::Confirm); present();
    assert(settings::take_exit()==settings::Exit::Library && !library::pending());
    assert(library::discards==discarded+1);
    library::save_ok=true;
    // System order and row actions use the UI, including confirmation defaults.
    root(); enter(3,settings::Page::System); enter(0,settings::Page::GameLibrary); back();
    focus(1); press(input::Confirm); press(input::PagePrevious); press(input::Confirm); present();
    assert(settings::take_exit()==settings::Exit::Disk);
    root(); enter(3,settings::Page::System); focus(3);
    press(input::Confirm); press(input::PagePrevious); press(input::Confirm); present();
    assert(settings::take_exit()==settings::Exit::Bootsel);
    root(); enter(3,settings::Page::System); focus(2);
    press(input::Confirm); snapshot("restore-defaults"); press(input::Confirm); present();
    assert(!resets && settings::active() && !settings::transitioning()); // Cancel is selected initially.
    press(input::Confirm); press(input::PagePrevious); back(); present();
    assert(!resets && !settings::transitioning()); // Back cancels too.
    // Formatting waits until after presentation and release, bypasses all saves,
    // and remains usable when State is already unavailable.
    configuration::brightness(35); library::edited=true; writable=false;
    const auto before_reset_saves=saves, before_reset_order_saves=library::saves;
    press(input::Confirm); press(input::PagePrevious); press(input::Confirm);
    assert(!resets && settings::transitioning());
    input::hub.local(input::LocalB); present();
    assert(!resets && settings::take_exit()==settings::Exit::None);
    configuration::brightness(40); // A late edit cannot restart the save gate.
    input::hub.local(0); present();
    assert(resets==1 && settings::take_exit()==settings::Exit::Defaults && !settings::active());
    assert(saves==before_reset_saves && library::saves==before_reset_order_saves);
    // A failed reset never reboots or reports a successful reset; retry works.
    root(); enter(3,settings::Page::System); focus(2); reset_ok=false;
    press(input::Confirm); press(input::PagePrevious); press(input::Confirm); present();
    assert(resets==2 && settings::active() && !settings::transitioning());
    assert(settings::take_exit()==settings::Exit::None && std::strstr(settings::notice(),"failed"));
    reset_ok=true;
    press(input::Confirm); press(input::PagePrevious); press(input::Confirm); present();
    assert(resets==3 && settings::take_exit()==settings::Exit::Defaults);
    // Home before release cancels the reset and uses the ordinary save gate.
    writable=true; root(); input::hub.local(input::LocalB);
    settings::request_exit(settings::Exit::Defaults); present();
    home(); input::hub.local(0); present();
    assert(resets==3 && settings::take_exit()==settings::Exit::Library);
    assert(!configuration::changed() && !library::pending());
    // The game list shares the same scrolling, clamps after deletions, and
    // returns to the first window after moving a game to the front.
    storage::count=17;
    root(); enter(3,settings::Page::System); enter(0,settings::Page::GameLibrary);
    enter(0,settings::Page::GameOrder);
    focus(6); press(input::PagePrevious); present();
    assert(frame[(74+3*24+6)*320+15]==ui::cyan);
    back(); enter(0,settings::Page::GameOrder); present();
    assert(settings::focus()==5 && frame[(74+3*24+6)*320+15]==ui::cyan);
    press(input::Confirm); present(); present();
    assert(settings::focus()==0 && frame[80*320+15]==ui::cyan);
    assert(frame[74*320+309]==ui::rgb(229,218,194));
    focus(16); storage::count=7; present();
    assert(settings::focus()==6 && frame[(74+4*24+6)*320+15]==ui::cyan);
    storage::count=3; present();
    assert(settings::focus()==2 && frame[(74+2*24+6)*320+15]==ui::cyan);
    assert(frame[74*320+309]==ui::rgb(16,20,24)); // No scrollbar for a short list.
    storage::count=0; present();
    assert(settings::focus()==0 && frame[80*320+15]!=ui::cyan);
    // Palette browsing is a draft: draw performs no I/O, Back/Home discard it,
    // Confirm joins the existing save-on-exit transaction.
    storage::config.palette=presentation::Palette::Current; configuration::init();
    root(); enter(0,settings::Page::Display); enter(2,settings::Page::Palette);
    snapshot("palette-current"); present(); const auto current_sample=frame;
    assert(frame[64*320+128]==ui::cyan);
    press(input::Next); snapshot("palette-unsaturated"); present();
    assert(frame!=current_sample && !configuration::changed());
    assert(storage::config.palette==presentation::Palette::Current);
    press(input::Next); snapshot("palette-smooth-v2"); back();
    assert(settings::page()==settings::Page::Display && !configuration::changed());
    enter(2,settings::Page::Palette); press(input::Next); press(input::PagePrevious);
    assert(settings::title_selected()); snapshot("palette-title"); present();
    assert(frame[64*320+128]==ui::dim);
    const auto title_sample=frame;
    press(input::Next); present(); assert(frame==title_sample); // Title focus cannot change colors.
    press(input::Confirm);
    assert(settings::page()==settings::Page::Display && !configuration::changed());
    enter(2,settings::Page::Palette); press(input::Next); press(input::Confirm);
    assert(settings::page()==settings::Page::Palette && configuration::changed());
    assert(storage::config.palette==presentation::Palette::Unsaturated);
    snapshot("palette-applied"); present(); assert(frame[64*320+128]==ui::rgb(70,220,100));
    press(input::PagePrevious); present(); assert(frame[64*320+128]==ui::dim);
    press(input::PageNext); present(); assert(frame[64*320+128]==ui::cyan);
    press(input::Next); press(input::Previous); present();
    assert(frame[64*320+128]==ui::cyan); // Returning to the applied value isn't another confirmation.
    const auto palette_saves=saves; home(); present();
    assert(settings::take_exit()==settings::Exit::Library && saves==palette_saves+1);
    root(); enter(0,settings::Page::Display); enter(2,settings::Page::Palette); present();
    const auto selected_sample=frame;
    assert(frame[64*320+128]==ui::cyan);
    press(input::Next); home(); present();
    assert(settings::take_exit()==settings::Exit::Library && saves==palette_saves+1);
    root(); enter(0,settings::Page::Display); enter(2,settings::Page::Palette); present();
    assert(frame==selected_sample);
    press(input::Next); press(input::Confirm); save_ok=false; home(); present();
    assert(settings::active() && configuration::changed() && storage::config.palette==presentation::Palette::SmoothV2);
    save_ok=true; press(input::Confirm); present();
    assert(settings::take_exit()==settings::Exit::Library && !configuration::changed());
    for(auto preset:{presentation::Palette::Sony,presentation::Palette::Nintendo,presentation::Palette::BmfFinal3}) {
        root(); enter(0,settings::Page::Display); enter(2,settings::Page::Palette);
        press(input::Next); snapshot(presentation::palette_id(preset)); present();
        assert(frame[64*320+128]==ui::cyan && !configuration::changed());
        press(input::Confirm); assert(storage::config.palette==preset);
        home(); present(); assert(settings::take_exit()==settings::Exit::Library && !configuration::changed());
    }
    root(); enter(0,settings::Page::Display); enter(2,settings::Page::Palette);
    press(input::Next); present(); assert(frame==current_sample); // Wrap to Original without applying.
    press(input::Previous); press(input::Confirm);
    assert(storage::config.palette==presentation::Palette::BmfFinal3 && !configuration::changed());
    // Controller testing uses raw normalized input, never menu mappings/turbo.
    storage::config.ble_enabled=true; storage::config.swap_ab=true; configuration::init();
    input::hub.local(0); input::hub.attach(input::Source::Ble,ble_pad);
    input::hub.enable(input::Source::Ble,true);
    gamepad::ControllerState pad;
    const auto tick=[&](uint32_t ms=1) {
        now+=ms; dispatch_input(now);
    };
    const auto report=[&](const gamepad::ControllerState &state) {ble_pad.update(state,now); tick();};
    pad.keys=gamepad::KeyA; report(pad);
    root(); enter(1,settings::Page::Control); enter(1,settings::Page::ControllerTest);
    tick(2000); assert(settings::page()==settings::Page::ControllerTest); // Opening A must be released first.
    report({}); snapshot("gamepad-idle"); present();
    const auto idle_pad=frame;
    pad={}; pad.left_x=255; report(pad); present();
    assert(frame[90*320+49]==idle_pad[90*320+49]); // Stick does not light the physical D-pad.
    assert(frame[174*320+135]==ui::cyan); // Positive endpoint reaches the circle, not a smaller square.
    pad.left_x=0; report(pad); present(); assert(frame[174*320+75]==ui::cyan);
    pad.left_x=255;
    pad.dpad=gamepad::North|gamepad::West;
    pad.keys=gamepad::KeyX|gamepad::KeyY|gamepad::KeyLB|gamepad::KeyRT|gamepad::KeyL3|gamepad::KeyR3|gamepad::KeyStart|gamepad::KeySelect;
    pad.right_x=0; pad.right_y=255; pad.lt=180; pad.rt=70;
    report(pad); snapshot("gamepad-active"); present();
    assert(frame[90*320+49]==ui::cyan && frame[110*320+27]==ui::cyan);
    tick(2000); assert(settings::page()==settings::Page::ControllerTest); // X/Y and Start/Select don't navigate or turbo.
    report({}); pad={}; pad.keys=gamepad::KeyB; report(pad); tick(600);
    snapshot("gamepad-exit-hold"); assert(settings::page()==settings::Page::ControllerTest);
    report({}); tick(1500); assert(settings::page()==settings::Page::ControllerTest); // A short B only tests B.
    report(pad); tick(1199); assert(settings::page()==settings::Page::ControllerTest);
    tick(1); assert(settings::page()==settings::Page::Control && settings::focus()==1);
    tick(2000); assert(settings::page()==settings::Page::Control); // Held exit cannot reopen the test.
    report({}); enter(1,settings::Page::ControllerTest);
    pad.keys=gamepad::KeyA; report(pad); tick(900);
    pad.keys=gamepad::KeyB; report(pad); tick(400);
    assert(settings::page()==settings::Page::ControllerTest); // A and B holds cannot be added together.
    ble_pad.release(); tick(); snapshot("gamepad-local"); tick(2000);
    assert(settings::page()==settings::Page::ControllerTest);
    report(pad); tick(2000); assert(settings::page()==settings::Page::ControllerTest); // Reconnect requires release.
    report({}); now=0xffffff00u; pad.keys=gamepad::KeyA; report(pad); tick(1200);
    assert(settings::page()==settings::Page::Control); // Hold timer wraps safely.
    report({}); ble_pad.release(); tick(); enter(1,settings::Page::ControllerTest);
    input::hub.local(input::LocalUp|input::LocalB); tick(); present();
    assert(frame[90*320+49]==ui::cyan && frame[137*320+260]==ui::rgb(76,218,151));
    tick(1200);
    assert(settings::page()==settings::Page::Control); // Local uses the same logical A hold.
    input::hub.local(0); tick(); enter(1,settings::Page::ControllerTest);
    home(); present(); assert(settings::take_exit()==settings::Exit::Library); // Global Home remains available.
    assert(!configuration::changed());
    // Background browsing is non-destructive until a separate confirmation.
    backgrounds={{"/rom/background/background.565",153600,0x1234},
        {"/rom/background/Sky.565",153600,0x2345},
        {"/system/assets/background/Sky.565",153600,0x3456},
        {"/system/assets/background/tiny.565",9280,0xffff},
        {"/system/assets/background/not-an-image.txt",153600,0xffff}};
    storage::config.menu_background[0]=0; configuration::init(); ui::load_background();
    root(); enter(0,settings::Page::Display);
    const auto before_scan=reads;
    enter(1,settings::Page::Backgrounds); assert(background::count()==3 && reads==before_scan);
    snapshot("background-list"); present(); assert(reads==before_scan);
    assert(!std::strcmp(background::name(1),"Sky.565") && !std::strcmp(background::source(2),"USB"));
    enter(0,settings::Page::BackgroundPreview); present(); assert(frame[100*320+160]==0x1234);
    press(input::Next); present(); snapshot("background-preview");
    assert(frame[100*320+160]==0x2345 && !configuration::changed());
    press(input::PagePrevious); assert(settings::title_selected());
    press(input::Confirm); assert(settings::page()==settings::Page::Backgrounds && settings::focus()==1);
    ui::background_image(frame.data()); assert(frame[0]==0x1234);
    enter(1,settings::Page::BackgroundPreview); press(input::Next); press(input::Confirm);
    assert(configuration::changed() && !std::strcmp(storage::config.menu_background,backgrounds[2].path.c_str()));
    const auto before_background_save=saves;
    assert(background::current()==2); present(); snapshot("background-applied");
    assert(frame[0]==ui::rgb(70,220,100));
    home(); present(); assert(settings::take_exit()==settings::Exit::Library);
    assert(saves==before_background_save+1 && !background::count() && !configuration::changed());
    ui::background_image(frame.data()); assert(frame[0]==0x3456);
    root(); enter(0,settings::Page::Display); enter(1,settings::Page::Backgrounds);
    assert(settings::focus()==2); enter(2,settings::Page::BackgroundPreview);
    press(input::Next); present(); assert(frame[100*320+160]==0x1234);
    home(); present(); settings::take_exit(); assert(saves==before_background_save+1);
    ui::background_image(frame.data()); assert(frame[0]==0x3456);
    root(); enter(0,settings::Page::Display); enter(1,settings::Page::Backgrounds);
    backgrounds[1].fail=true; enter(0,settings::Page::BackgroundPreview); press(input::Next);
    assert(settings::page()==settings::Page::Backgrounds && std::strstr(settings::notice(),"Cannot read"));
    ui::background_image(frame.data()); assert(frame[0]==0x3456 && !configuration::changed());
    backgrounds[1].fail=false;
    enter(0,settings::Page::BackgroundPreview); press(input::Confirm);
    assert(!storage::config.menu_background[0]); save_ok=false;
    home(); present(); assert(settings::active() && configuration::changed());
    save_ok=true; press(input::Confirm); present(); assert(settings::take_exit()==settings::Exit::Library);
    ui::background_image(frame.data()); assert(frame[0]==0x1234);
    std::strcpy(storage::config.menu_background,"/system/assets/background/missing.565");
    ui::load_background(); ui::background_image(frame.data());
    assert(frame[0]==0x1234 && ui::background_fallback());
    assert(std::strstr(storage::config.menu_background,"missing"));
    backgrounds.clear(); ui::load_background(); ui::background_image(frame.data()); assert(frame[0]==0);
    root(); enter(0,settings::Page::Display); enter(1,settings::Page::Backgrounds);
    press(input::Confirm); assert(settings::page()==settings::Page::Backgrounds);
    back(); assert(!background::count());
    for(unsigned i=70;i>0;--i) {
        char path[80]; std::snprintf(path,sizeof path,"/system/assets/background/%02u.565",i);
        backgrounds.push_back({path,153600,0});
    }
    assert(background::open() && background::limited() && background::count()==64);
    assert(!std::strcmp(background::name(0),"01.565") && !std::strcmp(background::name(63),"64.565"));
    background::close(); background_scan_ok=false;
    assert(!background::open() && !background::count()); background_scan_ok=true;
    for(const char *path:{"/rom/nes-background.565","/system/background/a.565","/rom/background/../a.565",
        "/rom/background/sub/a.565","/rom/background/a.png","/rom/background/.565"}) assert(!background::valid_path(path));
    assert(background::valid_path("/system/assets/background/Space name.565"));
    assert(background::valid_path("/rom/background/a.565"));
    backgrounds.clear(); storage::config.menu_background[0]=0; configuration::init();
    backgrounds={{"/rom/side-background/center/Same.565",15360,0x1111},
        {"/system/assets/side-background/center/Pair.565",30720,0x2222,false,true},
        {"/rom/side-background/side/Left.565",30720,0x3333},
        {"/rom/side-background/side/Right.565",30720,0x4444},
        {"/rom/side-background/side/Too narrow.565",15360,0xffff}};
    storage::config.game_position=presentation::Position::Center;
    root();enter(0,settings::Page::Display);enter(3,settings::Page::GameDisplay);
    focus(1);press(input::PageNext);assert(settings::focus()==1); // Center has no FPS row.
    enter(1,settings::Page::SideBackgrounds);assert(side_background::count()==3);
    enter(2,settings::Page::SidePreview);present();assert(side_background::view().width==32);
    assert(!storage::config.side_backgrounds[0][0]); // Preview is not an application.
    press(input::Confirm);assert(std::strstr(storage::config.side_backgrounds[0],"Same.565"));
    press(input::Previous);present();assert(side_background::view().width==64);
    assert(side_background::view().column(288)[100]==uint16_t(0x2222+100*64+32));
    back();back();assert(settings::page()==settings::Page::GameDisplay);
    enter(0,settings::Page::GamePosition);enter(1,settings::Page::GameDisplay);
    assert(storage::config.game_position==presentation::Position::Left);
    enter(1,settings::Page::SideBackgrounds);assert(side_background::count()==3);
    enter(1,settings::Page::SidePreview);press(input::Confirm);back();back();
    focus(2);press(input::Confirm);assert(storage::config.fps_monitor);
    enter(0,settings::Page::GamePosition);enter(0,settings::Page::GameDisplay);
    focus(1);press(input::PageNext);assert(settings::focus()==1);
    enter(1,settings::Page::SideBackgrounds);assert(settings::focus()==2);
    enter(0,settings::Page::SidePreview);press(input::Confirm);assert(!storage::config.side_backgrounds[0][0]);
    assert(storage::config.fps_monitor && !side_background::view().fps);
    assert(std::strstr(storage::config.side_backgrounds[1],"Left.565"));
    home();present();settings::take_exit();assert(!configuration::changed());
    storage::config.game_position=presentation::Position::Left;
    side_background::prepare();auto side=side_background::view();
    assert(side.width==64 && side.column(256)[100]==0x3333);
    storage::config.game_position=presentation::Position::Right;
    assert(side_background::open() && side_background::count()==3);
    assert(side_background::preview(1));side_background::apply();side_background::prepare();
    assert(side_background::view().column(0)[100]==0x3333 && side_background::view().fps);
    backgrounds[2].fail=true;side_background::prepare();assert(!side_background::view().pixels);
    backgrounds.clear();side_background::close();
}
