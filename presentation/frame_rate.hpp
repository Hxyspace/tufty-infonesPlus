#pragma once
#include <cstdint>
namespace presentation {
// Measure completed frame intervals, including publication, services and
// pacing. Accumulating unsigned timer deltas also handles 32-bit timer wrap.
class FrameRate {
 public:
  struct Sample {std::uint32_t frames,fps_x100;std::uint64_t elapsed_us;};
  void frame(std::uint32_t now){
    if(started_){elapsed_+=std::uint32_t(now-previous_);++frames_;}
    previous_=now;started_=true;
  }
  Sample sample(){
    const Sample result{frames_,elapsed_?std::uint32_t(std::uint64_t(frames_)*100000000/elapsed_):0,elapsed_};
    elapsed_=0;frames_=0;return result;
  }
 private:
  bool started_=false;
  std::uint32_t previous_=0,frames_=0;
  std::uint64_t elapsed_=0;
};
}
