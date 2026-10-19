#pragma once
#include <cstdint>
namespace diagnostics {
// Emulator/application thread only. Display-thread values live in display::stats.
inline bool playing=false;
inline std::uint32_t frames=0,work_last_us=0,work_max_us=0,service_gap_max_us=0;
inline void reset(){frames=work_last_us=work_max_us=service_gap_max_us=0;}
}
