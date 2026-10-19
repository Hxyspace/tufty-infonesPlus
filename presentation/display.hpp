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
void present_game(const presentation::GameFrame &screen);
void pause(bool enabled);
bool paused();
void brightness(uint8_t percent);
void prepare_reboot();
struct Stats {uint32_t frames,dropped,last_us,worst_us,te_timeouts;bool te_enabled;uint32_t transfer_us;};
Stats stats();
}
