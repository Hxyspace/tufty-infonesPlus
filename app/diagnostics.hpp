#pragma once
#include <cstdint>
#include "presentation/frame_rate.hpp"
namespace diagnostics {
// Emulator/application thread only. Display-thread values live in display::stats.
inline bool playing=false;
inline bool working=false;
inline std::uint32_t frames=0,work_last_us=0,work_max_us=0,service_gap_max_us=0;
inline std::uint32_t publish_last_us=0,publish_max_us=0;
inline std::uint32_t service_work_us=0,service_last_us=0,usb_max_us=0,ble_max_us=0;
inline presentation::FrameRate rate;
inline void reset(){
  frames=work_last_us=work_max_us=service_gap_max_us=0;
  publish_last_us=publish_max_us=0;
  service_work_us=service_last_us=usb_max_us=ble_max_us=0;
  working=false;rate={};
}
}
