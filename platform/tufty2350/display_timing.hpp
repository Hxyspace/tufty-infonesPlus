#pragma once
#include <cstdint>
#include <cmath>
namespace tufty::panel {
constexpr uint32_t kTeTimeoutUs = 50000;
struct SyncStatus { bool enabled = false, timed_out = false; uint32_t timeouts = 0; };
// Hardware-independent policy shared by the board driver and stuck-pin tests.
class Timing {
 public:
  void configure(bool enabled) { status_.enabled=enabled; status_.timed_out=false; }
  SyncStatus status() const { return status_; }
  template<class Clock, class Pin, class Idle>
  void wait(Clock now, Pin pin, Idle idle) {
    status_.timed_out=false;
    if (!status_.enabled) return;
    const uint32_t start=now();
    // If already high, wait for the next complete low->high edge. A stuck
    // high pin must not masquerade as a working VSYNC signal.
    bool low=!pin();
    for (;;) {
      if (uint32_t(now()-start)>=kTeTimeoutUs) {
        status_.timed_out=true;status_.enabled=false;
        if(status_.timeouts!=UINT32_MAX)++status_.timeouts;
        return;
      }
      const bool high=pin();
      if (!high) low=true;
      else if (low) return;
      idle();
    }
  }
 private:
  SyncStatus status_{};
};
inline uint16_t backlight_pwm(float level) {
  if (!std::isfinite(level) || level<=0) return 0;
  if(level>1)level=1;
  const auto brightness=static_cast<uint8_t>(level*255.0f);
  return uint16_t(8192+uint16_t(std::pow(float(brightness)/255.0f,2.8f)*(65535.0f-8192)+0.5f));
}
}
