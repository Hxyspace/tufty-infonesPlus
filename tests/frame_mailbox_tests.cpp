#include "presentation/frame_mailbox.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
using presentation::FrameMailbox;
int main(){
  // Explore every producer/consumer ordering, including same-slot reuse and
  // replacement while the consumer retains a frame for an arbitrary time.
  for(unsigned order=0;order<(1u<<13);++order){
    FrameMailbox box;
    std::array<uint32_t,3> payload{};
    uint32_t produced=UINT32_MAX-5,latest=0,held=0;
    bool fresh=false,have_held=false;
    for(unsigned step=0;step<13;++step){
      assert(box.writing()!=box.reading());
      if(order&(1u<<step)){
        payload[box.writing()]=++produced;latest=produced;
        assert(box.publish()==fresh);fresh=true;
      } else {
        assert(box.acquire()==fresh);
        if(fresh){held=latest;have_held=true;assert(payload[box.reading()]==latest);}
        fresh=false;
      }
      assert(box.pending()==fresh);
      if(have_held)assert(payload[box.reading()]==held);
    }
  }
  // Real concurrent access: yield while reading a payload, so premature reuse
  // or a half-published frame cannot hide behind atomic metadata alone.
  FrameMailbox box;
  struct Payload {uint32_t serial=0;std::array<uint32_t,128> words{};};
  std::array<Payload,3> slots;
  std::atomic<bool> done{false};
  std::thread producer([&]{
    for(uint32_t n=1;n<=100000;++n){
      auto &p=slots[box.writing()];p.serial=n;
      for(unsigned i=0;i<p.words.size();++i)p.words[i]=n*131+i;
      box.publish();
      if(n%47==0)std::this_thread::yield();
    }
    done.store(true,std::memory_order_release);
  });
  uint32_t last=0,consumed=0;
  for(;;){
    if(box.acquire()){
      const auto &p=slots[box.reading()];const auto n=p.serial;
      assert(n>last);last=n;++consumed;
      for(unsigned i=0;i<p.words.size();++i){
        assert(p.words[i]==n*131+i);
        if(i==64)std::this_thread::yield();
      }
    } else if(done.load(std::memory_order_acquire) && !box.pending())break;
  }
  producer.join();assert(consumed && last==100000);
}
