#include "usb.hpp"
#include "diagnostics.hpp"
#include "storage.hpp"
#include "fat_disk.hpp"
#include "display.hpp"
#include "console.hpp"
#include "power.hpp"
#include "ble.hpp"
#include "controllers.hpp"
#include "disk_ui.hpp"
#include "disk_activity.hpp"
#include "memory_stats.hpp"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdio/driver.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
namespace usb {
tgp::Receiver pad;
bool disk_requested=false,rescan_requested=false,save_requested=false,dirty=false;
static bool mode=false, owned=false, failed=false, ejected=false, prevent=false, eject_complete=false;
static uint32_t eject_at=0;
static bool console_tx_pending=false;
constexpr uint32_t kDiskMagic=0x4e45534d;
static DiskActivity disk_activity;
static DiskActivity write_activity;
static bool disk_write_paused=false;
static bool disk_burst_active=false;
static unsigned disk_back=1;
static disk_ui::State disk_drawn=disk_ui::State::Unavailable;
static bool disk_first=true;
static void disk_lights(bool enabled) {
    static bool lit=false;
    if(lit==enabled) return;
    lit=enabled;
    for(uint pin : {uint(BW_LED_0),uint(BW_LED_1),uint(BW_LED_2),uint(BW_LED_3)}) gpio_put(pin,enabled);
}
static void release_disk_display() {
    display::hold_disk(false);
    if(disk_write_paused) {
        display::pause(false);
        disk_write_paused=false;
    }
}
static void transfer_started(bool writing) {
    uint32_t now=to_ms_since_boot(get_absolute_time());
    disk_activity.begin(now);
    if(!writing) {
        // Reads suppress animation only; they never change the write indicator.
        display::hold_disk(true);
        return;
    }
    write_activity.begin(now);
    disk_lights(true);
    if(!disk_burst_active) {
        disk_burst_active=true;
        // Show the mode change once per write burst, before programming data.
        // This also works when an operation interrupts an idle frame's render:
        // that unfinished frame is abandoned by service_disk_frame(). Do not
        // re-enter TinyUSB from its own MSC callback while publishing this frame.
        release_disk_display();
        while(!display::disk_ready()) tight_loop_contents();
        disk_ui::draw(display::menu_buffers[disk_back],disk_ui::State::Transferring,0);
        display::present_disk(disk_back); disk_back^=1;
        while(!display::disk_ready()) tight_loop_contents();
        disk_drawn=disk_ui::State::Transferring; disk_first=false;
        display::hold_disk(true);
    }
}
static bool service_disk_frame() {
    poll();
    return !disk_activity.busy(to_ms_since_boot(get_absolute_time())) && !failed && !ejected;
}
template<size_t N> struct Queue {
    char data[N]; size_t head=0, used=0;
    bool add(const char *p,size_t n) {
        if(n>N-used) return false;
        for(size_t i=0;i<n;++i) data[(head+used+i)%N]=p[i];
        used+=n; return true;
    }
    void drain(uint8_t cdc) {
        if(!tud_cdc_n_connected(cdc)) {
            head=used=0;
            if(cdc==0) console_tx_pending=false;
            return;
        }
        size_t n=std::min({used,N-head,static_cast<size_t>(tud_cdc_n_write_available(cdc))});
        if(n) {
            size_t sent=tud_cdc_n_write(cdc,data+head,n); head=(head+sent)%N; used-=sent;
            if(cdc==0 && sent) console_tx_pending=true;
        }
        tud_cdc_n_write_flush(cdc);
    }
};
static Queue<2048> console_tx;
static Queue<512> protocol_tx;
static void flush_console() {
    std::fflush(stdout);
    uint32_t start=time_us_32();
    // A flush submits an asynchronous USB transfer. Keep servicing USB until
    // both queues and the last IN transfer finish, then give the terminal time
    // to display the reply. A disconnected/stalled host must not block reboot.
    while(tud_cdc_n_connected(0) && time_us_32()-start<1000000) {
        tud_task();
        console_tx.drain(0);
        if(!console_tx.used && !console_tx_pending &&
           tud_cdc_n_write_available(0)==CFG_TUD_CDC_TX_BUFSIZE && time_us_32()-start>=100000) break;
        sleep_ms(1);
    }
}
void transition_notice(const char *message) {
    std::fflush(stdout);
    // Preserve the final status even if earlier logs filled our software queue.
    size_t n=std::strlen(message);
    if(n>sizeof(console_tx.data)-console_tx.used) flush_console();
    console_tx.add(message,n);
    flush_console();
}
static bool send_protocol(const char *s,size_t n) {return protocol_tx.add(s,n);}
static void logs(const char *p,int n) {if(n>0) console_tx.add(p,n);}
static void echo_console(const char *p,size_t n) {console_tx.add(p,n);}
static void command(const char *s);
static console::Line console_line(echo_console,command);
static stdio_driver_t driver{};
bool disk_mode() {return mode;}
void init(bool disk) {
    mode=disk;
    if(!tusb_init()) panic("USB init");
    driver.out_chars=logs;
    stdio_set_translate_crlf(&driver,true);
    stdio_set_driver_enabled(&driver,true);
    pad.send=send_protocol;
}
static void command(const char *s) {
    if(power::pending() && std::strcmp(s,"power status")) {
        std::printf("POWER SLEEP: PENDING; command rejected\n"); return;
    }
    if(power::command(s) || controllers::command(s) || ble::command(s)) return;
    if(!std::strcmp(s,"help")) {
        std::printf("help | status | nes stats | lcd te [on|off] | usb disk | bootsel | rescan | save | brightness 1..100 | padid 1..65535 | swapab 0/1\n"
                    "power status | power sleep [seconds] | power idle 0..86400\n"
                    "pad status | serial on|off | ble status | ble on|off | ble scan\n"
                    "ble discover [stop] | ble devices | ble connect MAC public|random\n"
                    "ble whitelist [clear] | ble allow|forget|prefer MAC public|random | ble mode auto|whitelist\n"
                    "ble target auto | ble target MAC public|random | ble trace on|off\n");
    } else if(!std::strcmp(s,"status")) {
        std::printf("FAT=%u sector=%u State=%u ROMs=%u pad=%u packets=%lu rejected=%lu dirty=%u\n",
            storage::mounted(),flashfat::disk.sector_size,storage::state_ready(),unsigned(storage::count),pad.active,
            static_cast<unsigned long>(pad.received),static_cast<unsigned long>(pad.rejected),dirty);
    } else if(!std::strcmp(s,"nes stats")) {
        const auto d=display::stats();
        const auto rate=diagnostics::rate.sample();
        std::printf("NES frames=%lu work_us=%lu max_work_us=%lu max_poll_gap_us=%lu\n"
                    "NES fps=%lu.%02lu window_frames=%lu window_ms=%llu publish_us=%lu max_publish_us=%lu service_us=%lu max_usb_us=%lu max_ble_us=%lu\n"
                    "LCD frames=%lu drops=%lu frame_us=%lu max_frame_us=%lu TE=%u TE_requested=%u timeouts=%lu transfer_us=%lu\n",
            (unsigned long)diagnostics::frames,(unsigned long)diagnostics::work_last_us,
            (unsigned long)diagnostics::work_max_us,(unsigned long)diagnostics::service_gap_max_us,
            (unsigned long)(rate.fps_x100/100),(unsigned long)(rate.fps_x100%100),
            (unsigned long)rate.frames,(unsigned long long)(rate.elapsed_us/1000),
            (unsigned long)diagnostics::publish_last_us,(unsigned long)diagnostics::publish_max_us,
            (unsigned long)diagnostics::service_last_us,(unsigned long)diagnostics::usb_max_us,
            (unsigned long)diagnostics::ble_max_us,
            (unsigned long)d.frames,(unsigned long)d.dropped,(unsigned long)d.last_us,
            (unsigned long)d.worst_us,d.te_enabled,display::te_requested(),(unsigned long)d.te_timeouts,(unsigned long)d.transfer_us);
        static display::Stats previous;
        static uint32_t previous_at=0;
        static bool have_previous=false;
        const uint32_t now=time_us_32(),elapsed=have_previous?now-previous_at:0;
        const uint32_t frames=have_previous?d.frames-previous.frames:0;
        const uint32_t games=have_previous?d.game_frames-previous.game_frames:0;
        const uint32_t fps=elapsed?uint64_t(frames)*100000000/elapsed:0;
        const uint32_t game_fps=elapsed?uint64_t(games)*100000000/elapsed:0;
        std::printf("LCD fps=%lu.%02lu game_fps=%lu.%02lu window_frames=%lu window_game_frames=%lu window_ms=%lu wait_avg_us=%lu age_avg_us=%lu game_transfer_avg_us=%lu\n"
                    "LCD queue: published=%lu replaced=%lu discarded=%lu game_frames=%lu sequence=%lu publish_copy_bytes=0 wait_us=%lu max_wait_us=%lu age_us=%lu max_age_us=%lu interval_us=%lu max_interval_us=%lu\n"
                    "TE edges=%lu period_us=%lu min_period_us=%lu max_period_us=%lu high_us=%lu low_us=%lu ambiguous=%lu gaps=%lu\n",
            (unsigned long)(fps/100),(unsigned long)(fps%100),
            (unsigned long)(game_fps/100),(unsigned long)(game_fps%100),
            (unsigned long)frames,(unsigned long)games,(unsigned long)(elapsed/1000),
            (unsigned long)(frames?(d.wait_total_us-previous.wait_total_us)/frames:0),
            (unsigned long)(games?(d.age_total_us-previous.age_total_us)/games:0),
            (unsigned long)(games?(d.game_transfer_total_us-previous.game_transfer_total_us)/games:0),
            (unsigned long)d.published,(unsigned long)d.replaced,(unsigned long)d.discarded,
            (unsigned long)d.game_frames,(unsigned long)d.game_sequence,
            (unsigned long)d.wait_us,(unsigned long)d.wait_max_us,(unsigned long)d.age_us,(unsigned long)d.age_max_us,
            (unsigned long)d.interval_us,(unsigned long)d.interval_max_us,
            (unsigned long)d.te_edges,(unsigned long)d.te_period_us,(unsigned long)d.te_min_period_us,
            (unsigned long)d.te_max_period_us,(unsigned long)d.te_high_us,(unsigned long)d.te_low_us,
            (unsigned long)d.te_ambiguous,(unsigned long)d.te_gaps);
        previous=d;previous_at=now;have_previous=true;
        const auto memory=tufty::memory::stats();
        std::printf("MEM heap_used=%lu heap_free=%lu heap_unclaimed=%lu arena_used=%lu arena_peak=%lu small_fallbacks=%lu alloc_failures=%lu stack0_sample_min=%lu stack1_sample_min=%lu\n",
            (unsigned long)memory.heap_used,(unsigned long)memory.heap_free,(unsigned long)memory.heap_unclaimed,
            (unsigned long)memory.arena_used,(unsigned long)memory.arena_peak,
            (unsigned long)memory.small_fallbacks,(unsigned long)memory.allocation_failures,
            (unsigned long)memory.stack0_sample_min,(unsigned long)memory.stack1_sample_min);
    } else if(!std::strcmp(s,"lcd te") || !std::strcmp(s,"lcd te on") || !std::strcmp(s,"lcd te off")) {
        if(!std::strcmp(s,"lcd te on"))display::te_sync(true);
        else if(!std::strcmp(s,"lcd te off"))display::te_sync(false);
        const auto d=display::stats();
        std::printf("LCD TE: requested=%s active=%s timeouts=%lu\n",
            display::te_requested()?"on":"off",d.te_enabled?"on":"off",(unsigned long)d.te_timeouts);
    } else if(!std::strcmp(s,"usb disk")) {
        disk_requested=true; std::printf("USB DISK: REQUESTED\n");
    } else if(!std::strcmp(s,"bootsel")) {
        transition_notice("BOOTSEL: ENTERING; restarting into USB bootloader\r\n");
        display::prepare_reboot(); tud_disconnect(); reset_usb_boot(0,0);
    }
    else if(!std::strcmp(s,"rescan")) rescan_requested=true;
    else if(!std::strcmp(s,"save")) save_requested=true;
    else {
        unsigned value; char tail;
        if(std::sscanf(s,"brightness %u %c",&value,&tail)==1 && value>=1 && value<=100) {
            storage::config.brightness=value; display::brightness(value); dirty=storage::state_ready();
        } else if(std::sscanf(s,"padid %u %c",&value,&tail)==1 && value>=1 && value<=65535) {
            controllers::set_controller_id(uint16_t(value));
        } else if(std::sscanf(s,"swapab %u %c",&value,&tail)==1 && value<=1) {
            storage::config.swap_ab=value; dirty=storage::state_ready();
        } else std::printf("Unknown command; type help\n");
    }
}
void poll() {
    tud_task();
    static bool was_connected=false, console_connected=false;
    bool connected=tud_cdc_n_connected(1);
    if(connected!=was_connected) {
        pad.reset(); protocol_tx.head=protocol_tx.used=0;
        if(connected && !mode && storage::config.serial_enabled) pad.announce();
        was_connected=connected;
    }
    bool c0=tud_cdc_n_connected(0);
    if(c0!=console_connected) console_line.reset();
    if(!c0) tud_cdc_n_read_flush(0);
    if(c0 && !console_connected) std::printf("Tufty NES: CDC0 Console, CDC1 Gamepad%s\n",mode?" (disk mode)":"; type help");
    console_connected=c0;
    for(int budget=0;budget<128 && tud_cdc_n_available(0);++budget) {
        int c=tud_cdc_n_read_char(0);
        if(c0 && !mode && c>=0) {power::activity(); console_line.feed(static_cast<uint8_t>(c));}
    }
    uint32_t now=to_ms_since_boot(get_absolute_time());
    for(int budget=0;budget<256 && tud_cdc_n_available(1);++budget) {
        int c=tud_cdc_n_read_char(1);
        if(connected && !mode && storage::config.serial_enabled) {
            pad.byte(c,now);
            if(pad.input.contributing() && (pad.input.nes() || pad.input.home)) power::activity();
        }
    }
    if(!mode && storage::config.serial_enabled && pad.poll(now) && pad.input.contributing()) power::activity();
    console_tx.drain(0); protocol_tx.drain(1);
}
void shutdown() {
    pad.reset();
    stdio_set_driver_enabled(&driver,false);
    tud_disconnect(); tud_deinit(0);
}
void acquire_disk(bool valid) {
    owned=valid && storage::unmount_system();
    if(!owned) storage::unmount_system();
}
[[noreturn]] void reboot_disk() {
    transition_notice("USB DISK: ENTERING; restarting as USB mass storage\r\n");
    display::prepare_reboot();
    watchdog_hw->scratch[3]=kDiskMagic;
    tud_disconnect(); watchdog_reboot(0,0,10); while(true) tight_loop_contents();
}
[[noreturn]] void run_disk() {
    uint32_t previous=to_ms_since_boot(get_absolute_time());
    while(true) {
        poll();
        uint32_t now=to_ms_since_boot(get_absolute_time());
        if(!tud_mounted()) {disk_activity.reset(); write_activity.reset();}
        bool busy=owned && !failed && !ejected && disk_activity.busy(now);
        bool writing=owned && !failed && !ejected && write_activity.busy(now);
        disk_lights(writing);
        display::hold_disk(busy);
        if(!busy && disk_write_paused) {
            display::pause(false);
            disk_write_paused=false;
        }
        if(!writing) disk_burst_active=false;
        disk_ui::State state=failed?disk_ui::State::Failed:ejected?disk_ui::State::Ejected:
            !owned?disk_ui::State::Unavailable:writing?disk_ui::State::Transferring:disk_ui::State::Ready;
        bool changed=disk_first || state!=disk_drawn;
        if((!busy || changed) && (changed || display::disk_ready()) && (changed || now-previous>=33)) {
            // A write status change gets one frame even if directory reads
            // continue. Reads never prolong the write timer or relight LEDs.
            release_disk_display();
            while(!display::disk_ready()) tight_loop_contents();
            // Only reuse a buffer after core1 finishes its previous frame.
            // Animation publishes asynchronously; a status change finishes
            // once before holding the panel through the remaining file access.
            // An operation arriving mid-render abandons this back buffer.
            if(disk_ui::draw(display::menu_buffers[disk_back],state,now-previous,
                             busy || failed || ejected || !owned?nullptr:service_disk_frame)) {
                display::present_disk(disk_back); disk_back^=1;
                previous=now; disk_drawn=state; disk_first=false;
                if(busy) {
                    while(!display::disk_ready()) tight_loop_contents();
                    display::hold_disk(true);
                }
            }
        }
        if(eject_complete && time_us_32()-eject_at>=250000 && gpio_get(BTN_HOME)) {
            disk_lights(false);
            transition_notice("USB DISK: EJECTED; returning to menu\r\n");
            display::prepare_reboot();
            tud_disconnect(); watchdog_reboot(0,0,10);
            while(true) tight_loop_contents();
        }
        tight_loop_contents();
    }
}
bool consume_disk_request() {
    bool flag=watchdog_caused_reboot() && watchdog_hw->scratch[3]==kDiskMagic;
    watchdog_hw->scratch[3]=0; return flag;
}
}
extern "C" {
void tud_cdc_tx_complete_cb(uint8_t itf) {
    // TinyUSB invokes this before sending the next packet from its FIFO.
    // An empty FIFO here means the last data packet reached the USB host.
    if(itf==0) usb::console_tx_pending=tud_cdc_n_write_available(0)!=CFG_TUD_CDC_TX_BUFSIZE;
}
void tud_msc_inquiry_cb(uint8_t,uint8_t vendor[8],uint8_t product[16],uint8_t rev[4]) {
    std::memcpy(vendor,"TUFTY   ",8); std::memcpy(product,"NES SYSTEM      ",16); std::memcpy(rev,"0001",4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if(!lun && usb::owned && !usb::failed && !usb::ejected) return true;
    tud_msc_set_sense(lun,SCSI_SENSE_NOT_READY,0x3a,0); return false;
}
void tud_msc_capacity_cb(uint8_t,uint32_t *blocks,uint16_t *size) {
    *size=flashfat::disk.sector_size?flashfat::disk.sector_size:4096;
    *blocks=tufty::storage::layout::kSystem.bytes/(*size);
}
bool tud_msc_is_writable_cb(uint8_t lun) {return tud_msc_test_unit_ready_cb(lun);}
int32_t tud_msc_read10_cb(uint8_t lun,uint32_t lba,uint32_t offset,void *out,uint32_t bytes) {
    uint32_t at;
    if(!tud_msc_test_unit_ready_cb(lun) || !flashfat::disk.range(lba,offset,bytes,at)) return -1;
    usb::transfer_started(false);
    if(flashfat::disk.read(at,out,bytes)) return static_cast<int32_t>(bytes);
    usb::failed=true; usb::disk_lights(false);
    tud_msc_set_sense(lun,SCSI_SENSE_MEDIUM_ERROR,0x11,0); return -1;
}
int32_t tud_msc_write10_cb(uint8_t lun,uint32_t lba,uint32_t offset,uint8_t *in,uint32_t bytes) {
    uint32_t at;
    if(!tud_msc_test_unit_ready_cb(lun) || !flashfat::disk.range(lba,offset,bytes,at)) return -1;
    usb::transfer_started(true);
    uint32_t original=bytes;
    alignas(4) static uint8_t staging[4096];
    while(bytes) {
        uint32_t base=at&~4095u, within=at-base, n=std::min(bytes,4096-within);
        if(!flashfat::disk.read(base,staging,sizeof(staging))) {usb::failed=true; break;}
        if(std::memcmp(staging+within,in,n)) {
            std::memcpy(staging+within,in,n);
            // Drain once for the burst, then keep core1 parked through all
            // following sectors and the 500 ms quiet interval.
            if(!display::paused()) {
                display::pause(true);
                usb::disk_write_paused=true;
            }
            bool written=storage::replace_system_sector(base,staging);
            if(!written) {usb::failed=true; break;}
        }
        at+=n; in+=n; bytes-=n;
    }
    if(usb::failed) {usb::disk_lights(false); tud_msc_set_sense(lun,SCSI_SENSE_MEDIUM_ERROR,0x0c,0); return -1;}
    return original;
}
void tud_msc_read10_complete_cb(uint8_t) {
    usb::disk_activity.complete(to_ms_since_boot(get_absolute_time()));
}
void tud_msc_write10_complete_cb(uint8_t) {
    uint32_t now=to_ms_since_boot(get_absolute_time());
    usb::disk_activity.complete(now);
    usb::write_activity.complete(now);
}
bool tud_msc_start_stop_cb(uint8_t lun,uint8_t,bool start,bool load_eject) {
    if(load_eject && !start) {
        if(usb::prevent || usb::failed) {tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x53,2); return false;}
        usb::ejected=true;
    }
    return true;
}
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun,uint8_t prohibit,uint8_t) {
    if(lun) return false;
    usb::prevent=prohibit!=0;
    return true;
}
void tud_msc_scsi_complete_cb(uint8_t,const uint8_t cmd[16]) {
    if(cmd[0]==SCSI_CMD_START_STOP_UNIT && usb::ejected) {usb::eject_at=time_us_32(); usb::eject_complete=true;}
}
int32_t tud_msc_scsi_cb(uint8_t lun,const uint8_t cmd[16],void *,uint16_t) {
    if(cmd[0]==0x35 && !usb::failed) return 0; // SYNCHRONIZE CACHE(10)
    tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x20,0); return -1;
}
}
