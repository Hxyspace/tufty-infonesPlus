#pragma once
#include <cstdint>
namespace presentation {
// Rational deadlines preserve fractional microseconds and ignore display FPS.
// A lag beyond two frames resynchronizes; it never asks the core to skip time.
class FrameClock {
 public:
  void reset(std::uint32_t now,unsigned cpu_hz,unsigned ppu_dots,unsigned divider){
    at_=now;remainder_=0;
    numerator_=std::uint64_t(ppu_dots)*1000000u;
    denominator_=std::uint64_t(cpu_hz)*divider;
  }
  std::uint32_t next(std::uint32_t now){
    const auto total=numerator_+remainder_;
    const auto period=std::uint32_t(total/denominator_);
    remainder_=total%denominator_;at_+=period;
    if(std::int32_t(now-at_)>std::int32_t(period*2))at_=now;
    return at_;
  }
 private:
  std::uint32_t at_=0;
  std::uint64_t numerator_=1000000,denominator_=60,remainder_=0;
};
}
