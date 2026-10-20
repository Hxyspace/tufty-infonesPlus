#pragma once
#include <cstdint>
namespace tufty::memory {
struct Stats {
  uint32_t heap_used=0,heap_free=0,heap_unclaimed=0;
  uint32_t arena_used=0,arena_peak=0,small_fallbacks=0,allocation_failures=0;
  // Minimum observed SP margin at service/render sample sites, NOT a full
  // stack high-water measurement (an unsampled deeper call may use more).
  uint32_t stack0_sample_min=0,stack1_sample_min=0;
};
void sample_stack(unsigned core);
Stats stats(); // core0 only; malloc/arena also belong to core0.
}
