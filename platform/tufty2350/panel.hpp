#pragma once
#include <cstdint>
#include "display_timing.hpp"
namespace tufty::panel {
void early_init();
void init();
void brightness(std::uint8_t percent);
// Display/core1 only, between frames. Re-enabling also rearms timeout fallback.
void te_sync(bool enabled);
// Wait before choosing the next complete game frame. No RAMWR is issued yet.
void wait_for_frame();
// Display/core1 only. Suspend capture across Flash-safe pause gaps.
void capture_pause(bool paused);
EdgeStats edge_stats();
void begin_frame(bool sync=true);
// Returns with DMA in progress. Keep data alive until next send/end_frame;
// the caller can prepare its other column while the previous column drains.
void send_column(const std::uint16_t *data,unsigned pixels);
void end_frame();
void shutdown();
SyncStatus sync_status();
}
