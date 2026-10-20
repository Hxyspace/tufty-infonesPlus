#pragma once
#include <cstdint>
#include "game_frame.hpp"
namespace display {
constexpr int width=320, height=240;
extern uint16_t *menu_buffers[2];
extern uint16_t palette[512];
void early_init();
void start();
void present_menu(unsigned index);
// Alternate buffers only after the previous frame completes. Animation frames
// publish asynchronously; status transitions finish once before holding frames.
bool disk_ready();
void present_disk(unsigned index);
void hold_disk(bool enabled);
// core0 owns this writable SRAM slot until present_game() publishes it.
// The returned next slot may immediately be written; no full-frame copy occurs.
presentation::GameFrame &game_buffer();
presentation::GameFrame &present_game();
// core0 starts only after its library UI request completed; ends before
// requesting a UI frame. That completed UI boundary drains stale game work.
void begin_game();
void end_game();
void pause(bool enabled);
bool paused();
void brightness(uint8_t percent);
// Application/core0 requests; core1 applies after the current frame drains.
// Runtime only, defaults on at boot. Repeating on retries a timed-out TE pin.
void te_sync(bool enabled);
bool te_requested();
void prepare_reboot();
struct Stats {
    uint32_t frames=0,dropped=0,last_us=0,worst_us=0,te_timeouts=0;
    bool te_enabled=false;
    uint32_t transfer_us=0,published=0,replaced=0,discarded=0,game_frames=0;
    uint32_t wait_us=0,wait_max_us=0,wait_total_us=0;
    uint32_t age_us=0,age_max_us=0,age_total_us=0,interval_us=0,interval_max_us=0;
    uint32_t game_transfer_total_us=0,game_sequence=0;
    uint32_t te_edges=0,te_period_us=0,te_min_period_us=0,te_max_period_us=0;
    uint32_t te_high_us=0,te_low_us=0,te_ambiguous=0,te_gaps=0;
};
Stats stats();
}
