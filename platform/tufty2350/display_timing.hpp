#pragma once
#include <cstdint>
#include <cmath>
namespace tufty::panel {
constexpr uint32_t kTeTimeoutUs = 50000;
struct SyncStatus { bool enabled = false, timed_out = false; uint32_t timeouts = 0; };
struct EdgeStats {
  uint32_t rises=0,period_us=0,min_period_us=0,max_period_us=0;
  uint32_t high_us=0,low_us=0,ambiguous=0,gaps=0;
};
// Owned by the display-core GPIO ISR. Snapshot with that core's IRQs disabled.
// These are OBSERVED edges: interrupt masking can coalesce hardware events.
class TeEdges {
 public:
  void edge(bool rise,bool fall,uint32_t now) {
    if(rise && fall){++stats_.ambiguous;reset_sequence();return;}
    if(rise){
      ++stats_.rises;
      if(have_rise_){
        const uint32_t period=now-rise_at_;
        stats_.period_us=period;
        if(!stats_.min_period_us || period<stats_.min_period_us)stats_.min_period_us=period;
        if(period>stats_.max_period_us)stats_.max_period_us=period;
      }
      if(have_fall_)stats_.low_us=now-fall_at_;
      rise_at_=now;have_rise_=true;
    } else if(fall){
      if(have_rise_)stats_.high_us=now-rise_at_;
      fall_at_=now;have_fall_=true;
    }
  }
  void gap(){++stats_.gaps;reset_sequence();}
  EdgeStats stats() const{return stats_;}
 private:
  void reset_sequence(){have_rise_=have_fall_=false;}
  EdgeStats stats_;
  uint32_t rise_at_=0,fall_at_=0;
  bool have_rise_=false,have_fall_=false;
};
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
