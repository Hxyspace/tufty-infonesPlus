// PIO transport and panel setup adapted from tufty2350-native's MIT driver.
// See licenses/PIMORONI-LICENSE.
#include "display.hpp"
#include "panel.hpp"
#include "pixels.hpp"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <atomic>
#include <cstring>
#include <cmath>
#include <initializer_list>
void tufty_service();
namespace display {
static uint16_t __uninitialized_psram("menu") menus[2][width*height];
static presentation::GameFrame __uninitialized_psram("frames") games[2];
uint16_t *menu_buffers[2]={menus[0],menus[1]};
uint16_t palette[512]{};
static std::atomic<unsigned> requested{1}, completed{0};
static std::atomic<bool> ready{false}, pause_request{false}, pause_ack{false};
static std::atomic<bool> disk_hold{false};
static std::atomic<uint32_t> sent{0},dropped{0},last_us{0},worst_us{0},te_timeouts{0};
static std::atomic<uint32_t> transfer_us{0};
static std::atomic<bool> te_enabled{false};
static std::atomic<int> pending_te{-1}; // -1: no request, 0: off, 1: on/retry
static bool desired_te=true; // Application/core0 only.
// Only core0 publishes brightness. Keep the panel dark until its first UI frame.
static uint8_t desired_brightness=0;
static bool visible=false;
alignas(4) static uint16_t columns[2][height];
static uint16_t line_emphasis[height];
void early_init(){tufty::panel::early_init();}
void brightness(uint8_t percent){
    desired_brightness=percent>100?100:percent;
    if(visible)tufty::panel::brightness(desired_brightness);
}
void te_sync(bool enabled){
    desired_te=enabled;
    pending_te.store(enabled?1:0,std::memory_order_release);
}
bool te_requested(){return desired_te;}
// Keep the conversion loop out of XIP even when the compiler inlines a
// single-call-site helper; Flash and PSRAM share the external memory path.
static void __no_inline_not_in_flash_func(frame)(unsigned id) {
    const auto started=time_us_32();
    tufty::panel::begin_frame();
    const auto transfer_started=time_us_32();
    if(id>2)std::memcpy(line_emphasis,games[id-3].emphasis,sizeof line_emphasis);
    for(int x=0;x<width;++x) {
        uint16_t *col=columns[x&1];
        for(int y=0;y<height;++y) {
            uint16_t pixel=id<=2 ? menus[id-1][y*width+x] :
                x>=32 && x<288 ? palette[games[id-3].colors[x-32][y]|line_emphasis[y]] : 0;
            col[y]=__builtin_bswap16(pixel);
        }
        tufty::panel::send_column(col,height);
    }
    tufty::panel::end_frame();
    transfer_us.store(time_us_32()-transfer_started,std::memory_order_relaxed);
    const uint32_t elapsed=time_us_32()-started;
    last_us.store(elapsed,std::memory_order_relaxed);
    if(elapsed>worst_us.load(std::memory_order_relaxed))worst_us.store(elapsed,std::memory_order_relaxed);
    sent.fetch_add(1,std::memory_order_relaxed);
    const auto sync=tufty::panel::sync_status();
    te_timeouts.store(sync.timeouts,std::memory_order_relaxed);
    te_enabled.store(sync.enabled,std::memory_order_relaxed);
}
static void core() {
    flash_safe_execute_core_init();
    tufty::panel::init(); ready.store(true,std::memory_order_release);
    te_enabled.store(tufty::panel::sync_status().enabled,std::memory_order_relaxed);
    while(true) {
        if(pause_request.load(std::memory_order_acquire)) {
            pause_ack.store(true,std::memory_order_release);
            while(pause_request.load(std::memory_order_acquire)) tight_loop_contents();
            pause_ack.store(false,std::memory_order_release);
        }
        // Only core1 touches Timing, between fully drained frames. Do not
        // interrupt a TE wait, DMA transfer or the Flash-safe pause handshake.
        if(pending_te.load(std::memory_order_acquire)!=-1){
            const int change=pending_te.exchange(-1,std::memory_order_acq_rel);
            tufty::panel::te_sync(change==1);
            te_enabled.store(tufty::panel::sync_status().enabled,std::memory_order_relaxed);
        }
        unsigned id=requested.load(std::memory_order_acquire);
        if(!disk_hold.load(std::memory_order_acquire) && completed.load(std::memory_order_relaxed)!=id){
            frame(id);completed.store(id,std::memory_order_release);
        }
        tight_loop_contents();
    }
}
void start() {
    for(unsigned i=0;i<512;++i)palette[i]=presentation::token_rgb565(i);
    std::memset(menus,0,sizeof(menus)); std::memset(games,0,sizeof(games));
    multicore_launch_core1(core);
    while(!ready.load(std::memory_order_acquire)) {tufty_service(); tight_loop_contents();}
}
void present_menu(unsigned index) {
    unsigned id=1+(index&1); requested.store(id,std::memory_order_release);
    // Keep USB/radio polling while core1 renders; no alarm wake is needed.
    while(completed.load(std::memory_order_acquire)!=id) {tufty_service(); tight_loop_contents();}
    if(!visible) {visible=true; tufty::panel::brightness(desired_brightness);}
}
bool disk_ready() {
    return completed.load(std::memory_order_acquire)==requested.load(std::memory_order_acquire);
}
void hold_disk(bool enabled) {disk_hold.store(enabled,std::memory_order_release);}
void present_disk(unsigned index) {
    // Caller owns the other buffer after disk_ready(). Core1 handles the
    // transfer while core0 immediately returns to servicing USB requests.
    requested.store(1+(index&1),std::memory_order_release);
    if(!visible) {visible=true; tufty::panel::brightness(desired_brightness);}
}
void present_game(const presentation::GameFrame &screen) {
    unsigned old=requested.load(std::memory_order_acquire);
    if(completed.load(std::memory_order_acquire)!=old){dropped.fetch_add(1,std::memory_order_relaxed);return;}
    unsigned index=old==3?1:0;
    std::memcpy(&games[index],&screen,sizeof screen);
    requested.store(3+index,std::memory_order_release);
}
void pause(bool enabled) {
    pause_request.store(enabled,std::memory_order_release);
    while(pause_ack.load(std::memory_order_acquire)!=enabled) tight_loop_contents();
}
bool paused() {return pause_ack.load(std::memory_order_acquire);}
Stats stats(){return {sent.load(),dropped.load(),last_us.load(),worst_us.load(),te_timeouts.load(),te_enabled.load(),transfer_us.load()};}
void prepare_reboot() {
    visible=false;
    if(!ready.load(std::memory_order_acquire)) {early_init(); return;}
    // Ack is issued only after a full frame and the final DMA/PIO drain.
    // Also works when disk mode has already paused the display.
    pause(true);
    tufty::panel::shutdown();
}
}
