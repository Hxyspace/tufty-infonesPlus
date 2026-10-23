#include "settings.hpp"
#include "configuration.hpp"
#include "controllers.hpp"
#include "ble.hpp"
#include "power.hpp"
#include "menu_repeat.hpp"
#include "ui.hpp"
#include "battery.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#ifndef TUFTY_VERSION
#define TUFTY_VERSION "unknown"
#endif

namespace settings {
namespace {
enum class Confirm {None,DisableBle,DisableSerial,Search,ModeAuto,ModeAllowed,Clear,Remove,Sleep,Disk,Bootsel};
enum class Dialog {None,Confirm,SaveError,Brightness,SleepTime,ControllerId};
Page current=Page::Root;
unsigned focuses[unsigned(Page::Count)]{};
bool opened=false,searching=false,search_flow=false,saving=false,waiting_release=false;
bool search_started=false;
bool save_bypassed=false;
bool title_focus=false;
Exit target=Exit::None,completed=Exit::None;
Confirm confirmation=Confirm::None;
Dialog dialog=Dialog::None;
bool adjusting() {return dialog==Dialog::Brightness || dialog==Dialog::SleepTime;}
unsigned choice=0;
menu::Repeat repeat;
uint32_t search_at=0,last_discovery_sample=0;
uint32_t input_epoch=0,drawn_now=0,message_at=0;
bool previous_ble=true;
bluetooth::Policy previous_allowed;
ble::Candidate devices[bluetooth::Manager::Capacity];
size_t device_count=0,discovery_count=0;
bluetooth::Peer selected_peer;
bool selected_from_search=false;
bool selected_tgp=false;
uint16_t selected_controller_id=0;
char selected_name[32]{},message[96]{},question[64]{};
uint16_t id_original=0;
unsigned id_draft=0,digit=0;
enum class Mark {None,Off,On};
struct Row {char label[80]{},value[32]{}; bool arrow=false; Mark mark=Mark::None;};
Row rows[17];
unsigned row_count=0;
char footer[96]{};
const char *titles[]={"Settings","Display","Control","Bluetooth","Search gamepads","Gamepad",
    "Allowed devices","Allowed gamepad","Connection mode","Power","System","About"};
constexpr Page categories[]={Page::Display,Page::Control,Page::Power,Page::System,Page::About};
constexpr ui::Icon category_icons[]={ui::Icon::Display,ui::Icon::Control,ui::Icon::Power,ui::Icon::System,ui::Icon::About};
constexpr uint32_t sleep_presets[]={0,60,180,300,600,1800};
bool category(Page page) {
    for(auto item:categories) if(item==page) return true;
    return false;
}
unsigned &selected() {return focuses[unsigned(current)];}
void barrier() {repeat.reset(); input::hub.clear_events(); input_epoch=input::hub.epoch();}
void say(const char *text) {std::snprintf(message,sizeof(message),"%s",text); message_at=drawn_now;}
void stop_search() {
    if(search_flow) ble::stop_discovery();
    search_flow=false; searching=false;
}
void go(Page next) {
    for(unsigned i=0;i<5;++i) if(categories[i]==next) focuses[unsigned(Page::Root)]=i;
    current=next; title_focus=false; message[0]=0; barrier();
    if(next==Page::Allowed) previous_allowed=storage::config.ble;
}
void ask(Confirm action,const char *text) {
    confirmation=action; dialog=Dialog::Confirm; choice=1;
    std::snprintf(question,sizeof(question),"%s",text); barrier();
}
void start_search(uint32_t now) {
    if(!storage::config.ble_enabled) return;
    if(!ble::discover()) {say("Search unavailable; try again"); return;}
    search_flow=true; searching=true; device_count=discovery_count=0;
    search_started=false;
    search_at=last_discovery_sample=now; focuses[unsigned(Page::Search)]=0; go(Page::Search);
}
void search(uint32_t now) {
    const auto state=ble::status().state;
    if(state==ble::State::Ready || state==ble::State::Connecting || state==ble::State::Discovering)
        ask(Confirm::Search,"Disconnect to search?");
    else start_search(now);
}
void peer_row(const bluetooth::Peer &peer,const char *name) {
    if(row_count>=17) return;
    auto &row=rows[row_count++]; char mac[18]; peer.text(mac);
    std::snprintf(row.label,sizeof(row.label),"%s %c %s",mac,peer.random?'R':'P',name?name:""); row.arrow=true;
}
void add(const char *label,const char *value="",bool arrow=false) {
    if(row_count>=17) return;
    auto &row=rows[row_count++];
    std::snprintf(row.label,sizeof(row.label),"%s",label);
    std::snprintf(row.value,sizeof(row.value),"%s",value); row.arrow=arrow;
}
void marked(const char *label,bool enabled) {
    add(label); rows[row_count-1].mark=enabled?Mark::On:Mark::Off;
}
const char *parent_title() {
    switch(current) {
    case Page::Root: return "Library";
    case Page::Bluetooth: return "Control";
    case Page::Search: case Page::Allowed: case Page::Mode: return "Bluetooth";
    case Page::Device: return "Search gamepads";
    case Page::AllowedDevice: return "Allowed devices";
    default: return "Settings";
    }
}
const char *connection() {
    const auto status=ble::status();
    if(selected_tgp && selected_controller_id!=storage::config.controller) return "TGP ID differs; change Controller ID";
    if(!(status.peer==selected_peer)) return "Not connected";
    if(status.state==ble::State::Ready) {
        if(!input::hub.status(input::Source::Ble).available) return "Link ready; awaiting input";
        return input::hub.active_remote()==input::Source::Ble?"Ready; active input":"Ready; input on standby";
    }
    if(status.state==ble::State::Connecting || status.state==ble::State::Discovering) return "Connecting...";
    if(status.state==ble::State::Error || status.state==ble::State::Backoff || status.last_error) return "Connection failed";
    return "Not connected";
}
void model(uint32_t now=0,Page view=Page::Count) {
    if(view==Page::Count) view=current;
    for(auto &row:rows) row={};
    row_count=0; footer[0]=0;
    const auto &cfg=storage::config;
    char value[40]{};
    switch(view) {
    case Page::Root:
        for(auto item:categories) add(titles[unsigned(item)],"",true);
        std::snprintf(footer,sizeof(footer),"%s",!configuration::available()?"Session only / Saving unavailable":
            configuration::changed()?"Changes save on exit":"Saved settings"); break;
    case Page::Display: {
        std::snprintf(value,sizeof(value),"%u%%",cfg.brightness); add("Brightness",value,true);
        marked("TE sync",display::te_requested());
        const auto stats=display::stats();
        std::snprintf(footer,sizeof(footer),"TE: %s (this session)",
            stats.te_enabled?(display::te_requested()?"Active":"Pending"):
            !display::te_requested()?"Off":stats.te_timeouts?"Fallback; confirm to retry":"Pending"); break;
    }
    case Page::Control:
        marked("Bluetooth",cfg.ble_enabled);
        rows[row_count-1].arrow=true;
        marked("Serial pad",cfg.serial_enabled); marked("Swap A/B",cfg.swap_ab);
        std::snprintf(footer,sizeof(footer),"Active input: %s",input::name(input::hub.active_remote())); break;
    case Page::Bluetooth:
        marked("Enabled",cfg.ble_enabled);
        if(cfg.ble_enabled) add("Search gamepads","",true);
        for(const char *label:{"Allowed devices","Connection mode","Controller ID"}) add(label,"",true);
        break;
    case Page::Search:
        if(searching) {
            add(search_started?"Searching...":"Starting search...");
            const auto seconds=search_started?(now-search_at)/1000:0;
            std::snprintf(value,sizeof(value),"%lu s / %u found",(unsigned long)(seconds<10?10-seconds:0),unsigned(discovery_count));
            add(value); std::snprintf(footer,sizeof(footer),"Up: return title; Home: exit");
        } else {
            for(size_t i=0;i<device_count;++i) peer_row(devices[i].peer,devices[i].name);
            add("Search again");
            if(!device_count) std::snprintf(footer,sizeof(footer),"No gamepads found");
        } break;
    case Page::Device: case Page::AllowedDevice: {
        if(cfg.ble_enabled) add("Connect");
        if(view==Page::Device) {add("Allow"); add("Allow & prefer");}
        else {
            const int at=cfg.ble.find(selected_peer);
            add(at>=0 && cfg.ble.preferred==at?"Clear preference":"Set preferred"); add("Remove");
        }
        char mac[18]; selected_peer.text(mac); add(mac,selected_peer.random?"Random":"Public");
        if(selected_name[0]) add(selected_name);
        std::snprintf(footer,sizeof(footer),"%s",connection()); break;
    }
    case Page::Allowed:
        for(unsigned i=0;i<cfg.ble.count;++i) {
            peer_row(cfg.ble.peers[i],""); rows[row_count-1].mark=i==cfg.ble.preferred?Mark::On:Mark::Off;
        }
        add("Clear allowed devices");
        std::snprintf(footer,sizeof(footer),"%u/4 allowed (not bonded)",cfg.ble.count); break;
    case Page::Mode:
        marked("Auto",cfg.ble.mode==bluetooth::Mode::AutoCompatible);
        marked("Allowed only",cfg.ble.mode==bluetooth::Mode::WhitelistOnly); break;
    case Page::Power:
        if(cfg.menu_sleep_seconds%60) std::snprintf(value,sizeof(value),"%lu s",(unsigned long)cfg.menu_sleep_seconds);
        else std::snprintf(value,sizeof(value),"%lu min",(unsigned long)(cfg.menu_sleep_seconds/60));
        if(cfg.menu_sleep_seconds) add("Auto sleep",value); else marked("Auto sleep",false);
        rows[row_count-1].arrow=true;
        add("Sleep now","",true); break;
    case Page::System:
        add("USB disk mode","",true); add("Firmware update","",true); break;
    case Page::About:
        add("Tufty NES",TUFTY_VERSION);
        std::snprintf(value,sizeof(value),"%u",unsigned(storage::count)); add("Games",value);
        add("Author","yuan");
        add("Config storage",configuration::available()?"Available":"Unavailable");
        if(battery::reading().valid) std::snprintf(value,sizeof(value),"~%u%%",battery::reading().percent);
        else std::strcpy(value,"--");
        add("Battery estimate",value); break;
    case Page::Count: break;
    }
    auto &focus=focuses[unsigned(view)];
    focus=std::min(focus,row_count?row_count-1:0);
}
void back() {
    if(dialog!=Dialog::None) {dialog=Dialog::None; target=Exit::None; barrier(); return;}
    switch(current) {
    case Page::Root: request_exit(Exit::Library); break;
    case Page::Bluetooth: go(Page::Control); break;
    case Page::Search: stop_search(); go(Page::Bluetooth); break;
    case Page::Device: go(Page::Search); break;
    case Page::AllowedDevice: go(Page::Allowed); break;
    case Page::Allowed: case Page::Mode: go(Page::Bluetooth); break;
    default: go(Page::Root); break;
    }
}
void confirmed(uint32_t now) {
    const auto action=confirmation; dialog=Dialog::None; barrier();
    switch(action) {
    case Confirm::DisableBle: controllers::set_enabled(input::Source::Ble,false); break;
    case Confirm::DisableSerial: controllers::set_enabled(input::Source::Serial,false); break;
    case Confirm::Search: start_search(now); break;
    case Confirm::ModeAuto: controllers::set_mode(bluetooth::Mode::AutoCompatible); break;
    case Confirm::ModeAllowed: controllers::set_mode(bluetooth::Mode::WhitelistOnly); break;
    case Confirm::Clear: controllers::whitelist_clear(); break;
    case Confirm::Remove: controllers::whitelist_remove(selected_peer); go(Page::Allowed); break;
    case Confirm::Sleep: request_exit(Exit::Sleep); break;
    case Confirm::Disk: request_exit(Exit::Disk); break;
    case Confirm::Bootsel: request_exit(Exit::Bootsel); break;
    case Confirm::None: break;
    }
}
void adjust(int direction) {
    if(dialog==Dialog::Brightness) {
        const int value=std::clamp(int(storage::config.brightness)+direction*5,1,100);
        configuration::brightness(unsigned(value));
    } else if(dialog==Dialog::SleepTime) {
        const auto previous=storage::config.menu_sleep_seconds;
        uint32_t next=previous;
        if(direction>0) {
            for(auto item:sleep_presets) if(item>previous) {next=item; break;}
        } else {
            next=0; for(auto item:sleep_presets) if(item<previous) next=item;
        }
        configuration::idle(next); power::activity();
    }
}
void activate(uint32_t now) {
    const auto row=selected(); auto &cfg=storage::config;
    switch(current) {
    case Page::Root: {
        go(categories[row]); break;
    }
    case Page::Display:
        if(row==0) {dialog=Dialog::Brightness; barrier();}
        else if(row==1) {
            const auto stats=display::stats();
            display::te_sync(display::te_requested() && !stats.te_enabled && stats.te_timeouts?true:!display::te_requested());
        } break;
    case Page::Control: {
        if(row==0) go(Page::Bluetooth);
        else if(row==1) {
            if(cfg.serial_enabled && input::hub.active_remote()==input::Source::Serial) ask(Confirm::DisableSerial,"Disable active serial pad?");
            else {controllers::set_enabled(input::Source::Serial,!cfg.serial_enabled); barrier();}
        } else configuration::swap(!cfg.swap_ab);
        break;
    }
    case Page::Bluetooth:
        if(row==0) {
            if(cfg.ble_enabled && input::hub.active_remote()==input::Source::Ble) ask(Confirm::DisableBle,"Disable active Bluetooth?");
            else {controllers::set_enabled(input::Source::Ble,!cfg.ble_enabled); barrier();}
        }
        else if(cfg.ble_enabled && row==1) search(now);
        else if(row==(cfg.ble_enabled?2u:1u)) go(Page::Allowed);
        else if(row==(cfg.ble_enabled?3u:2u)) go(Page::Mode);
        else {
            id_original=cfg.controller; id_draft=id_original; digit=0;
            dialog=Dialog::ControllerId; message[0]=0; barrier();
        }
        break;
    case Page::Search:
        if(searching) break;
        if(row==device_count) search(now);
        else {
            selected_peer=devices[row].peer; selected_from_search=true;
            selected_tgp=devices[row].tgp; selected_controller_id=devices[row].controller_id;
            std::snprintf(selected_name,sizeof(selected_name),"%s",devices[row].name);
            go(Page::Device);
            if(devices[row].tgp && devices[row].controller_id!=cfg.controller) say("TGP controller ID differs");
        } break;
    case Page::Allowed:
        if(row==cfg.ble.count) ask(Confirm::Clear,"Clear all allowed devices?");
        else {selected_peer=cfg.ble.peers[row]; selected_name[0]=0; selected_from_search=false; selected_tgp=false; go(Page::AllowedDevice);}
        break;
    case Page::Device: case Page::AllowedDevice: {
        const unsigned action=row+(cfg.ble_enabled?0:1);
        if(action==0) {
            if(ble::connect(selected_peer)) say("Connect requested; awaiting link"); else say("Connection request rejected");
        } else if(action==1) {
            if(selected_from_search) say(controllers::whitelist_add(selected_peer)?"Allowed; saves on exit":"List full; remove a device first");
            else {
                const int at=cfg.ble.find(selected_peer);
                if(at<0) say("Device removed externally");
                else {controllers::prefer(cfg.ble.preferred==at?nullptr:&selected_peer); say("Preference updated");}
            }
        } else if(action==2) {
            if(selected_from_search) say(controllers::whitelist_add(selected_peer,true)?"Allowed and preferred":"List full; remove a device first");
            else ask(Confirm::Remove,"Remove allowed device?");
        } break;
    }
    case Page::Mode: {
        const auto mode=row?bluetooth::Mode::WhitelistOnly:bluetooth::Mode::AutoCompatible;
        if(mode!=cfg.ble.mode) {
            if(ble::status().state==ble::State::Ready || input::hub.active_remote()==input::Source::Ble)
                ask(row?Confirm::ModeAllowed:Confirm::ModeAuto,"Change mode and reconnect?");
            else {controllers::set_mode(mode); barrier();}
        } break;
    }
    case Page::Power:
        if(row==0) {dialog=Dialog::SleepTime; barrier();}
        else if(row==1) ask(Confirm::Sleep,"Sleep until next wake?");
        break;
    case Page::System:
        ask(row?Confirm::Bootsel:Confirm::Disk,row?"Restart in firmware updater?":"Restart as a USB disk?"); break;
    default: break;
    }
}
}
void open() {
    opened=true; current=Page::Root; completed=target=Exit::None;
    focuses[unsigned(Page::Root)]=0;
    title_focus=false;
    dialog=Dialog::None; saving=waiting_release=save_bypassed=false; message[0]=0;
    previous_ble=storage::config.ble_enabled; previous_allowed=storage::config.ble; barrier();
}
bool active() {return opened;}
Page page() {return current;}
unsigned focus() {return selected();}
bool title_selected() {return title_focus;}
bool editing_id() {return dialog==Dialog::ControllerId;}
const char *notice() {return message;}
bool transitioning() {return target!=Exit::None;}
void request_exit(Exit destination) {
    if(target!=Exit::None) return;
    if(!opened) open();
    stop_search(); dialog=Dialog::None; target=destination;
    save_bypassed=false;
    saving=configuration::changed() || !configuration::available();
    waiting_release=!saving; say(saving?"Saving...":"Release buttons to continue"); barrier();
}
void request_save() {
    if(target!=Exit::None) return;
    const bool was_open=opened;
    if(!opened) open();
    target=was_open?Exit::Stay:Exit::ResumeLibrary;
    save_bypassed=false;
    saving=true; waiting_release=false; say("Saving..."); barrier();
}
Exit take_exit() {const auto result=completed; completed=Exit::None; return result;}
void update(input::Snapshot keys,bool home,uint32_t now) {
    if(!opened) return;
    drawn_now=now; configuration::sync();
    if(message[0] && now-message_at>=5000 && !transitioning()) message[0]=0;
    const auto &allowed=storage::config.ble;
    bool list_changed=previous_allowed.count!=allowed.count;
    for(unsigned i=0;!list_changed && i<allowed.count;++i)
        list_changed=!(previous_allowed.peers[i]==allowed.peers[i]);
    if(list_changed && current==Page::Allowed) {
        // Preserve peer identity across Console edits, not its previous row.
        const int at=selected()<previous_allowed.count?allowed.find(previous_allowed.peers[selected()]):-1;
        selected()=at>=0?unsigned(at):std::min(selected(),unsigned(allowed.count));
        if(dialog==Dialog::Confirm) dialog=Dialog::None;
        barrier(); // Do not reuse a confirmation for a different list.
    }
    previous_allowed=allowed;
    if(current==Page::AllowedDevice && allowed.find(selected_peer)<0 && !transitioning()) {
        dialog=Dialog::None; go(Page::Allowed); say("Device removed externally");
    }
    if(previous_ble!=storage::config.ble_enabled) {
        previous_ble=storage::config.ble_enabled;
        // Hidden rows must not receive an edge intended for their old action.
        // Configuration-only pages remain available with the radio disabled.
        if(!previous_ble && (current==Page::Search || current==Page::Device)) {
            stop_search(); go(Page::Bluetooth);
        }
        if(current==Page::Bluetooth || current==Page::AllowedDevice)
            selected()=editing_id()?(previous_ble?4u:3u):0;
        if(!transitioning() && !adjusting() && !editing_id()) dialog=Dialog::None;
        barrier(); // Keep an outstanding save-error decision intact.
    }
    if(searching) {
        if(!search_started && ble::status().state==ble::State::Scanning) {search_started=true; search_at=now;}
        if(ble::status().state==ble::State::Error) {
            stop_search(); device_count=0; say("Search failed; try again"); barrier();
        }
        if(searching && (now-last_discovery_sample>=250 || !ble::status().discovering)) {
            // Count without changing the fixed result snapshot until completion.
            discovery_count=ble::candidate_count(); last_discovery_sample=now;
        }
        if(searching && !ble::status().discovering) {
            device_count=ble::candidates(devices,16); searching=false; barrier();
        }
    }
    model(now);
    if(home) {
        // Home always leaves the application, including a pending system
        // transition. Preserve its save/error gate and require button release.
        if(target!=Exit::None) {stop_search(); target=Exit::Library; barrier();}
        else request_exit(Exit::Library);
        return;
    }
    if(saving || waiting_release) return;
    if(keys.epoch!=input_epoch) {barrier(); return;}
    if(keys.pressed&input::Back) {back(); return;}
    const auto pressed=repeat.poll(keys,now);
    if(dialog!=Dialog::None) {
        if(editing_id()) {
            if(pressed&input::Previous) digit=digit?digit-1:0;
            else if(pressed&input::Next) digit=std::min(4u,digit+1);
            else if(pressed&(input::PagePrevious|input::PageNext)) {
                constexpr unsigned places[]={10000,1000,100,10,1}; const auto place=places[digit];
                const unsigned old=id_draft/place%10;
                const unsigned next=pressed&input::PagePrevious?std::min(9u,old+1):(old?old-1:0);
                id_draft=id_draft-old*place+next*place;
            } else if(pressed&input::Confirm) {
                if(storage::config.controller!=id_original) say("ID changed externally; reopen editor");
                else if(id_draft<1 || id_draft>65535) say("ID must be 1..65535");
                else {
                    controllers::set_controller_id(uint16_t(id_draft));
                    dialog=Dialog::None; message[0]=0; barrier();
                }
            }
            return;
        }
        if(adjusting()) {
            if(pressed&input::Previous) adjust(-1);
            else if(pressed&input::Next) adjust(1);
            else if(pressed&input::Confirm) {dialog=Dialog::None; barrier();}
            return;
        }
        const unsigned max_choice=dialog==Dialog::SaveError?2:1;
        if(pressed&input::PagePrevious) choice=choice?choice-1:0;
        else if(pressed&input::PageNext) choice=std::min(max_choice,choice+1);
        else if(pressed&input::Confirm) {
            if(dialog==Dialog::Confirm) {
                if(choice==0) confirmed(now); else {dialog=Dialog::None; barrier();}
            } else if(choice==0) {dialog=Dialog::None; saving=true; say("Saving..."); barrier();}
            else if(choice==1) {dialog=Dialog::None; waiting_release=true; save_bypassed=true; say("Session only; release buttons"); barrier();}
            else {dialog=Dialog::None; target=Exit::None; message[0]=0; barrier();}
        }
        return;
    }
    if(title_focus) {
        if(pressed&input::PageNext) {title_focus=false; selected()=0;}
        else if(!(pressed&(input::PagePrevious|input::Previous|input::Next)) && (pressed&input::Confirm)) back();
    } else if(pressed&input::PagePrevious) {
        if(selected()) --selected();
        else if(current!=Page::Root && !category(current)) title_focus=true;
    } else if(pressed&input::Previous) {
        if(keys.pressed&input::Previous) {
            if(category(current)) go(Page::Root);
            else if(current!=Page::Root) title_focus=true;
        }
    } else if(!searching) {
        if(pressed&input::PageNext) selected()=std::min(row_count?row_count-1:0,selected()+1);
        else if(pressed&input::Next) {
            // Enter once on an edge; a held Right cannot drill through pages.
            if(rows[selected()].arrow) {if(keys.pressed&input::Next) activate(now);}
        }
        else if(pressed&input::Confirm) activate(now);
    }
    model(now);
}
void after_present() {
    if(!opened) return;
    if(saving) {
        const auto result=configuration::save(); saving=false;
        std::printf("State save: %s\n",result==configuration::SaveResult::Saved?"OK":
            result==configuration::SaveResult::Unchanged?"unchanged":
            result==configuration::SaveResult::Unavailable?"unavailable; runtime settings retained":"failed; original record retained");
        if(result==configuration::SaveResult::Failed || result==configuration::SaveResult::Unavailable) {
            dialog=Dialog::SaveError; choice=0;
            say(result==configuration::SaveResult::Failed?"Save failed; settings retained":"Saving unavailable; session only"); barrier();
        } else {waiting_release=true; say("Release buttons to continue");}
    }
    // Console remains live while waiting for release. Never silently lose a
    // change received after the successful write but before a mode transition.
    if(waiting_release && !save_bypassed && configuration::changed()) {
        waiting_release=false; saving=true; say("Saving..."); return;
    }
    if(waiting_release && input::hub.neutral()) {
        if(target==Exit::Stay) {
            target=Exit::None; waiting_release=false; barrier();
            say(!configuration::available() || configuration::changed()?"Session only; changes retained":"Saved settings"); return;
        }
        completed=target; target=Exit::None; waiting_release=false; opened=false; barrier();
    }
}
void draw(uint16_t *out) {
    // Root is the category-column focus; its selected category is previewed
    // with exactly the same model as the active property column.
    const bool sidebar_focus=current==Page::Root;
    const bool split=sidebar_focus || category(current);
    const Page view=sidebar_focus?categories[focuses[unsigned(Page::Root)]]:current;
    model(drawn_now,view);
    const unsigned row_focus=focuses[unsigned(view)];
    constexpr auto background=ui::rgb(8,12,16),surface=ui::rgb(16,20,24),
        highlight=ui::rgb(43,57,72),separator=ui::rgb(116,109,97),green=ui::rgb(70,220,100),
        header=ui::rgb(184,176,160),footer_color=ui::rgb(171,163,146),
        edge=ui::rgb(229,218,194),bar_text=ui::rgb(39,42,40);
    constexpr int bar=ui::bar_height,bottom=240-bar;
    std::fill(out,out+320*240,background);
    ui::box(out,0,0,320,bar,header);
    ui::box(out,0,bottom,320,bar,footer_color);
    ui::box(out,0,bar,320,1,edge);
    ui::box(out,0,bar+1,320,1,separator);
    ui::box(out,0,bottom-1,320,1,edge);
    ui::box(out,0,bottom,320,1,separator);
    if(!ui::icon(out,ui::Icon::Header,8,(bar-20)/2)) ui::text_centered(out,"NES",8,0,bar,bar_text,34);
    ui::text_centered(out,"Settings",48,0,bar,bar_text,210);
    ui::status_icons(out,ui::BatteryStyle::Settings);
    // The body starts below the independent title band. Its panel keeps
    // six pixels clear on every side, regardless of the number of rows.
    constexpr int body_top=62,panel_margin=6,row_top=74,row_height=24;
    constexpr unsigned visible_rows=5;
    const int content_x=split?106:1;
    const int left=content_x+12,label_x=left+8;
    if(split) {
        ui::box(out,1,bar+2,102,bottom-bar-3,surface);
        ui::box(out,0,bar,1,bottom-bar,edge);
        ui::box(out,103,bar+1,1,bottom-bar-2,edge);
        ui::box(out,104,bar+1,1,bottom-bar-2,separator);
        ui::box(out,105,bar+1,1,bottom-bar-2,edge);
        for(unsigned i=0;i<5;++i) {
            const int y=39+33*i;
            const bool chosen=categories[i]==view;
            if(chosen) {
                ui::rounded_box(out,3,y,97,30,4,sidebar_focus?highlight:background);
                if(sidebar_focus) ui::box(out,4,y+9,2,12,ui::cyan);
            }
            ui::icon(out,category_icons[i],8,y+5);
            ui::text_centered(out,titles[unsigned(categories[i])],34,y,30,chosen?ui::white:ui::grey,65);
        }
        ui::box(out,106,bar+2,213,27,ui::rgb(16,24,33));
        ui::box(out,106,61,213,1,separator);
        ui::text_centered(out,titles[unsigned(view)],label_x,bar+2,27,ui::white,190);
    } else {
        ui::box(out,1,bar+2,317,27,ui::rgb(16,24,33));
        if(title_focus) ui::rounded_box(out,7,bar+4,21,23,4,highlight);
        ui::box(out,1,61,317,1,separator);
        ui::text_centered(out,"<",14,bar+2,27,ui::cyan,12);
        ui::text_centered(out,titles[unsigned(current)],30,bar+2,27,ui::white,280);
    }
    const unsigned first=row_focus>=visible_rows?row_focus-(visible_rows-1):0;
    const unsigned last=std::min(row_count,first+visible_rows);
    ui::rounded_box(out,content_x+panel_margin,body_top+panel_margin,
        318-content_x-2*panel_margin,bottom-1-body_top-2*panel_margin,5,surface);
    for(unsigned i=first;i<last;++i) {
        const int y=row_top+row_height*(i-first); const auto &row=rows[i];
        const bool focus=!sidebar_focus && !title_focus && i==row_focus && view!=Page::About && !searching;
        if(focus) {ui::rounded_box(out,left,y+1,306-left,22,5,highlight); ui::box(out,left+2,y+6,2,12,ui::cyan);}
        if(i+1<last) ui::box(out,label_x,y+23,298-label_x,1,separator);
        const int value_width=std::min(ui::width(row.value),std::max(0,285-label_x-12));
        const int label_right=row.value[0]?285-value_width-8:row.mark!=Mark::None?275:row.arrow?287:299;
        ui::text_centered(out,row.label,label_x,y,24,focus?ui::white:ui::grey,label_right-label_x);
        if(row.value[0]) ui::text_centered(out,row.value,285-value_width,y,24,ui::cyan,value_width);
        if(row.mark!=Mark::None) ui::circle(out,285,y+12,4,row.mark==Mark::On?green:ui::rgb(100,100,100));
        if(row.arrow) ui::text_centered(out,">",295,y,24,ui::orange,10);
    }
    if(row_count>visible_rows) {
        constexpr int track_height=visible_rows*row_height;
        const int thumb=std::max(6,int(visible_rows*track_height/row_count));
        const int offset=int(first*(track_height-thumb)/(row_count-visible_rows));
        ui::box(out,309,row_top,2,track_height,separator);
        ui::box(out,309,row_top+offset,2,thumb,ui::dim);
    }
    ui::box(out,0,bar,1,bottom-bar,edge);
    ui::box(out,318,bar+1,1,bottom-bar-2,separator);
    ui::box(out,319,bar,1,bottom-bar,edge);
    ui::box(out,0,bottom-1,320,1,edge);
    if(sidebar_focus && !message[0]) {
        constexpr int cy=bottom+bar/2;
        ui::box(out,6,cy-2,21,5,bar_text); ui::box(out,14,cy-10,5,21,bar_text);
        ui::text_centered(out,"Navigate",30,bottom,bar,bar_text,82);
        ui::circle(out,127,cy,10,ui::rgb(16,20,24));
        ui::text_centered(out,"A",127-ui::width("A")/2,cy-10,20,ui::white,14);
        ui::text_centered(out,"Select",140,bottom,bar,bar_text,72);
        ui::circle(out,230,cy,10,ui::rgb(16,20,24));
        ui::text_centered(out,"B",230-ui::width("B")/2,cy-10,20,ui::white,14);
        ui::text_centered(out,"Back",243,bottom,bar,bar_text,66);
    } else {
        const char *hint=title_focus?"Confirm: back; Home: exit":footer;
        if(!hint[0] && dialog==Dialog::None) hint=split?"Left / B: categories; Home: exit":parent_title();
        ui::text_centered(out,message[0]?message:hint,12,bottom,bar,bar_text,296);
    }
    if(dialog!=Dialog::None) {
        constexpr int h=bottom-bar-12,top=bar+6;
        ui::rounded_box(out,10,top,300,h,9,separator); ui::rounded_box(out,11,top+1,298,h-2,8,surface);
        if(editing_id()) {
            ui::text_centered(out,"Controller ID",28,top+12,24,ui::white,264);
            ui::text_centered(out,"Shared with serial pad",28,top+42,24,ui::grey,264);
            char value[6]; std::snprintf(value,sizeof(value),"%05u",id_draft);
            for(unsigned i=0;i<5;++i) {
                const int x=80+32*i;
                const char number[]={value[i],0};
                ui::text(out,number,x+(24-ui::width(number,ui::Font::Title))/2,top+78,
                    i==digit?ui::white:ui::cyan,24,ui::Font::Title);
                if(i==digit) ui::box(out,x,top+110,24,2,ui::orange);
            }
            return;
        }
        if(adjusting()) {
            const bool brightness=dialog==Dialog::Brightness;
            ui::text_centered(out,brightness?"Brightness":"Auto sleep",28,top+12,24,ui::white,264);
            char value[32]; const auto seconds=storage::config.menu_sleep_seconds;
            if(brightness) std::snprintf(value,sizeof(value),"%u%%",storage::config.brightness);
            else if(!seconds) std::strcpy(value,"Off");
            else if(seconds%60) std::snprintf(value,sizeof(value),"%lu s",(unsigned long)seconds);
            else std::snprintf(value,sizeof(value),"%lu min",(unsigned long)(seconds/60));
            ui::text_centered(out,value,160-ui::width(value)/2,top+54,32,ui::cyan,200);
            // Sleep presets occupy equally spaced detents. Existing custom
            // values interpolate between neighbours until explicitly changed.
            const auto &stops=sleep_presets;
            constexpr int track_x=45,track_width=230,track_y=110;
            int position=0;
            if(brightness) position=(storage::config.brightness-1)*(track_width-1)/99;
            else {
                unsigned slot=0; while(slot<5 && seconds>stops[slot+1]) ++slot;
                position=slot==5?track_width-1:int((slot*1000+(seconds-stops[slot])*1000/(stops[slot+1]-stops[slot]))*(track_width-1)/5000);
            }
            ui::box(out,track_x,top+track_y,track_width,3,separator);
            ui::box(out,track_x,top+track_y,position+1,3,ui::cyan);
            if(!brightness) for(int i=0;i<6;++i) ui::box(out,track_x+i*(track_width-1)/5,top+track_y-2,1,7,ui::dim);
            ui::circle(out,track_x+position,top+track_y+1,4,ui::white);
            ui::text_centered(out,"<",28,top+track_y-10,24,ui::orange,12);
            ui::text_centered(out,">",281,top+track_y-10,24,ui::orange,12);
            return;
        }
        ui::text_centered(out,dialog==Dialog::SaveError?"Settings not saved":question,28,top+12,24,ui::white,263);
        if(dialog==Dialog::SaveError) ui::text_centered(out,"Runtime changes are kept",28,top+38,24,ui::grey,263);
        const char *options[]={"Retry",target==Exit::Library || target==Exit::ResumeLibrary?
            "Return without saving":"Continue without saving","Cancel"};
        const char *confirm_options[]={"Continue","Cancel"};
        const unsigned options_count=dialog==Dialog::SaveError?3:2;
        for(unsigned i=0;i<options_count;++i) {
            const int y=top+h-14-26*options_count+26*i;
            if(i==choice) {ui::rounded_box(out,23,y,274,23,5,highlight); ui::box(out,25,y+6,2,11,ui::cyan);}
            ui::text_centered(out,dialog==Dialog::SaveError?options[i]:confirm_options[i],32,y,23,i==choice?ui::white:ui::dim,260);
        }
    }
}
}
