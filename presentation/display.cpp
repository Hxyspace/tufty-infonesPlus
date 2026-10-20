// PIO transport and panel setup adapted from tufty2350-native's MIT driver.
// See licenses/PIMORONI-LICENSE.
#include "display.hpp"
#include "frame_mailbox.hpp"
#include "panel.hpp"
#include "pixels.hpp"
#include "memory_stats.hpp"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <atomic>
#include <cstring>
void tufty_service();
namespace display {
static uint16_t __uninitialized_psram("menu") menus[2][width*height];
struct GamePacket { presentation::GameFrame pixels; uint32_t sequence=0,ready_at=0; };
// Keep all hot production AND consumption in SRAM. The producer publishes a
// complete slot, never copies a live working frame into external memory.
static GamePacket games[3];
static presentation::FrameMailbox mailbox;
static std::atomic<bool> game_active{false};
static uint32_t published=0,replaced=0; // Producer/core0 only.
uint16_t *menu_buffers[2]={menus[0],menus[1]};
uint16_t palette[512]{};
// UI work has a sequence independent of the alternating buffer index.
static std::atomic<uint32_t> requested{1},completed{0},menu_index{0};
static std::atomic<bool> ready{false},pause_request{false},pause_ack{false},disk_hold{false};
static std::atomic<uint32_t> sent{0},discarded{0},game_sent{0},last_us{0},worst_us{0},te_timeouts{0};
static std::atomic<uint32_t> transfer_us{0},wait_us{0},wait_max{0},wait_total{0};
static std::atomic<uint32_t> age_us{0},age_max{0},age_total{0},interval_us{0},interval_max{0};
static std::atomic<uint32_t> game_transfer_total{0},game_sequence{0};
static std::atomic<uint32_t> te_edges{0},te_period{0},te_min{0},te_max{0},te_high{0},te_low{0},te_ambiguous{0},te_gaps{0};
static std::atomic<bool> te_enabled{false};
static std::atomic<int> pending_te{-1}; // -1: no request, 0: off, 1: on/retry
static bool desired_te=true; // Application/core0 only.
static uint8_t desired_brightness=0;
static bool visible=false;
alignas(4) static uint16_t columns[2][height];
static uint16_t line_emphasis[height];
void early_init(){tufty::panel::early_init();}
void brightness(uint8_t percent){
    desired_brightness=percent>100?100:percent;
    if(visible)tufty::panel::brightness(desired_brightness);
}
void te_sync(bool enabled){desired_te=enabled;pending_te.store(enabled?1:0,std::memory_order_release);}
bool te_requested(){return desired_te;}
static void maximum(std::atomic<uint32_t> &value,uint32_t sample){
    if(sample>value.load(std::memory_order_relaxed))value.store(sample,std::memory_order_relaxed);
}
static void sync_stats(){
    const auto sync=tufty::panel::sync_status();
    te_timeouts.store(sync.timeouts,std::memory_order_relaxed);
    te_enabled.store(sync.enabled,std::memory_order_relaxed);
    const auto e=tufty::panel::edge_stats();
    te_edges.store(e.rises,std::memory_order_relaxed);te_period.store(e.period_us,std::memory_order_relaxed);
    te_min.store(e.min_period_us,std::memory_order_relaxed);te_max.store(e.max_period_us,std::memory_order_relaxed);
    te_high.store(e.high_us,std::memory_order_relaxed);te_low.store(e.low_us,std::memory_order_relaxed);
    te_ambiguous.store(e.ambiguous,std::memory_order_relaxed);te_gaps.store(e.gaps,std::memory_order_relaxed);
}
// Called only by the consumer between fully drained frames. Discarding a
// pending slot returns the OLD consumer slot, never a slot still being read.
static void discard_game(){if(mailbox.acquire())discarded.fetch_add(1,std::memory_order_relaxed);}
// Keep the conversion loop in SRAM, with DMA reading the other internal column.
static void __no_inline_not_in_flash_func(frame)(const uint16_t *menu,const GamePacket *game,uint32_t started,uint32_t waited) {
    tufty::memory::sample_stack(1);
    tufty::panel::begin_frame(false); // TE has already been waited for.
    const auto transfer_started=time_us_32();
    static uint32_t previous_game_at=0;
    static bool have_game_at=false;
    if(game){
        std::memcpy(line_emphasis,game->pixels.emphasis,sizeof line_emphasis);
        const uint32_t age=transfer_started-game->ready_at;
        age_us.store(age,std::memory_order_relaxed);maximum(age_max,age);
        age_total.fetch_add(age,std::memory_order_relaxed);
        if(have_game_at){
            const uint32_t interval=transfer_started-previous_game_at;
            interval_us.store(interval,std::memory_order_relaxed);maximum(interval_max,interval);
        }
        previous_game_at=transfer_started;have_game_at=true;
        game_sequence.store(game->sequence,std::memory_order_relaxed);
    } else have_game_at=false;
    for(int x=0;x<width;++x){
        uint16_t *col=columns[x&1];
        for(int y=0;y<height;++y){
            const uint16_t pixel=menu?menu[y*width+x]:
                x>=32 && x<288?palette[game->pixels.colors[x-32][y]|line_emphasis[y]]:0;
            col[y]=__builtin_bswap16(pixel);
        }
        tufty::panel::send_column(col,height);
    }
    tufty::panel::end_frame(); // Final DMA AND PIO must drain before recycling.
    const uint32_t transferred=time_us_32()-transfer_started;
    transfer_us.store(transferred,std::memory_order_relaxed);
    if(game){game_transfer_total.fetch_add(transferred,std::memory_order_relaxed);game_sent.fetch_add(1,std::memory_order_relaxed);}
    wait_us.store(waited,std::memory_order_relaxed);maximum(wait_max,waited);
    wait_total.fetch_add(waited,std::memory_order_relaxed);
    const uint32_t elapsed=time_us_32()-started;
    last_us.store(elapsed,std::memory_order_relaxed);maximum(worst_us,elapsed);
    sent.fetch_add(1,std::memory_order_relaxed);
    sync_stats();
}
static void core(){
    flash_safe_execute_core_init();
    tufty::panel::init();sync_stats();ready.store(true,std::memory_order_release);
    while(true){
        if(pause_request.load(std::memory_order_acquire)){
            tufty::panel::capture_pause(true);
            pause_ack.store(true,std::memory_order_release);
            while(pause_request.load(std::memory_order_acquire))tight_loop_contents();
            tufty::panel::capture_pause(false);
            pause_ack.store(false,std::memory_order_release);
        }
        if(pending_te.load(std::memory_order_acquire)!=-1){
            const int change=pending_te.exchange(-1,std::memory_order_acq_rel);
            tufty::panel::te_sync(change==1);sync_stats();
        }
        if(!game_active.load(std::memory_order_acquire))discard_game();
        const auto ui=requested.load(std::memory_order_acquire);
        if(!disk_hold.load(std::memory_order_acquire) &&
           (completed.load(std::memory_order_relaxed)!=ui || mailbox.pending())){
            const auto started=time_us_32();
            tufty::panel::wait_for_frame();
            const uint32_t waited=time_us_32()-started;
            // A request received during TE wait must be acknowledged BEFORE
            // starting a transfer. No buffer has been claimed at this point.
            if(pause_request.load(std::memory_order_acquire) || disk_hold.load(std::memory_order_acquire))continue;
            const auto next_ui=requested.load(std::memory_order_acquire);
            if(completed.load(std::memory_order_relaxed)!=next_ui){
                discard_game();
                frame(menus[menu_index.load(std::memory_order_relaxed)],nullptr,started,waited);
                completed.store(next_ui,std::memory_order_release);
            } else if(game_active.load(std::memory_order_acquire) && mailbox.acquire()){
                // Late latch: take the newest COMPLETE frame after TE, not
                // the frame that happened to be ready when waiting started.
                frame(nullptr,&games[mailbox.reading()],started,waited);
            }
        }
        tight_loop_contents();
    }
}
void start(){
    for(unsigned i=0;i<512;++i)palette[i]=presentation::token_rgb565(i);
    std::memset(menus,0,sizeof menus);
    multicore_launch_core1(core);
    while(!ready.load(std::memory_order_acquire)){tufty_service();tight_loop_contents();}
}
static uint32_t request_menu(unsigned index){
    menu_index.store(index&1,std::memory_order_relaxed);
    return requested.fetch_add(1,std::memory_order_release)+1;
}
void present_menu(unsigned index){
    // Do not overwrite a pending UI request/index (including startup frame).
    while(!disk_ready()){tufty_service();tight_loop_contents();}
    const auto sequence=request_menu(index);
    while(completed.load(std::memory_order_acquire)!=sequence){tufty_service();tight_loop_contents();}
    if(!visible){visible=true;tufty::panel::brightness(desired_brightness);}
}
bool disk_ready(){return completed.load(std::memory_order_acquire)==requested.load(std::memory_order_acquire);}
void hold_disk(bool enabled){disk_hold.store(enabled,std::memory_order_release);}
void present_disk(unsigned index){
    request_menu(index); // Caller checked disk_ready() and owns this buffer.
    if(!visible){visible=true;tufty::panel::brightness(desired_brightness);}
}
presentation::GameFrame &game_buffer(){return games[mailbox.writing()].pixels;}
void begin_game(){game_active.store(true,std::memory_order_release);}
void end_game(){game_active.store(false,std::memory_order_release);}
presentation::GameFrame &present_game(){
    auto &packet=games[mailbox.writing()];
    packet.sequence=++published;packet.ready_at=time_us_32();
    if(mailbox.publish())++replaced;
    return game_buffer();
}
void pause(bool enabled){
    pause_request.store(enabled,std::memory_order_release);
    while(pause_ack.load(std::memory_order_acquire)!=enabled)tight_loop_contents();
}
bool paused(){return pause_ack.load(std::memory_order_acquire);}
Stats stats(){
    Stats s{};
    s.frames=sent.load();s.dropped=replaced;s.last_us=last_us.load();s.worst_us=worst_us.load();
    s.te_timeouts=te_timeouts.load();s.te_enabled=te_enabled.load();s.transfer_us=transfer_us.load();
    s.published=published;s.replaced=replaced;s.discarded=discarded.load();s.game_frames=game_sent.load();
    s.wait_us=wait_us.load();s.wait_max_us=wait_max.load();s.wait_total_us=wait_total.load();
    s.age_us=age_us.load();s.age_max_us=age_max.load();s.age_total_us=age_total.load();
    s.interval_us=interval_us.load();s.interval_max_us=interval_max.load();
    s.game_transfer_total_us=game_transfer_total.load();s.game_sequence=game_sequence.load();
    s.te_edges=te_edges.load();s.te_period_us=te_period.load();s.te_min_period_us=te_min.load();s.te_max_period_us=te_max.load();
    s.te_high_us=te_high.load();s.te_low_us=te_low.load();s.te_ambiguous=te_ambiguous.load();s.te_gaps=te_gaps.load();
    return s;
}
void prepare_reboot(){
    visible=false;
    if(!ready.load(std::memory_order_acquire)){early_init();return;}
    pause(true);tufty::panel::shutdown();
}
}
