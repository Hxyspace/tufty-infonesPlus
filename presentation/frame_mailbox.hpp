#pragma once
#include <atomic>

namespace presentation {
// Single producer, single consumer. Each side exclusively owns one slot;
// the third belongs to the mailbox. Only a complete producer slot is handed
// over. The consumer returns its old slot only AFTER its final read/DMA drain.
// Payloads (including sequence/timestamp) live outside this index exchange.
class FrameMailbox {
 public:
  unsigned writing() const { return writing_; } // Producer only.
  unsigned reading() const { return reading_; } // Consumer only.
  bool pending() const { return (middle_.load(std::memory_order_acquire)&kNew)!=0; }
  // Producer: publishes writing(), acquires a slot that no consumer can read.
  // True means an older, never consumed frame was replaced.
  bool publish() {
    const unsigned old=middle_.exchange(writing_|kNew,std::memory_order_acq_rel);
    writing_=old&kIndex;
    return (old&kNew)!=0;
  }
  // Consumer: call only after finishing reading(). Claim as late as possible,
  // e.g. AFTER waiting for TE, so a newer pending frame can replace an old one.
  bool acquire() {
    if(!pending()) return false;
    const unsigned old=middle_.exchange(reading_,std::memory_order_acq_rel);
    reading_=old&kIndex;
    return true;
  }
 private:
  static constexpr unsigned kIndex=3,kNew=4;
  static_assert(std::atomic<unsigned>::is_always_lock_free);
  std::atomic<unsigned> middle_{2};
  unsigned writing_=0,reading_=1;
};
}
