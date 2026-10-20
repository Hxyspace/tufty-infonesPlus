// Run the actual presentation/display.cpp on two threads. The panel mock gates
// TE and final drain separately, checking that buffers stay immutable until DMA
// has consumed them and that pending frames are claimed AFTER the TE gate.
#include "presentation/display.hpp"
#include "presentation/pixels.hpp"
#include "platform/tufty2350/panel.hpp"
#include "platform/tufty2350/memory_stats.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
namespace {
struct Stop {};
std::mutex mutex;
std::condition_variable cv;
std::thread consumer;
std::atomic<bool> stop{false},pause_spinning{false};
thread_local bool pauser=false;
unsigned waits=0,ends=0,pauses=0;
bool pulse=false,block_end=false,in_end=false,active=false;
uint32_t timer=0;
tufty::panel::SyncStatus sync_state{true,false,0};
std::vector<std::array<uint16_t,320*240>> images;
std::array<uint16_t,320*240> image;
const uint16_t *dma_pointer=nullptr;
std::array<uint16_t,240> dma_copy;
unsigned column=0;
template<class Predicate> void until(Predicate predicate){
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!predicate()){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
}
template<class Predicate> void await_locked(std::unique_lock<std::mutex> &lock,Predicate predicate){
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!predicate()){
    assert(std::chrono::steady_clock::now()<deadline);
    lock.unlock();std::this_thread::yield();lock.lock();
  }
}
void wait_gate(unsigned count){
  std::unique_lock<std::mutex> lock(mutex);
  await_locked(lock,[&]{return waits>=count;});
  assert(waits==count);
}
void edge(){std::lock_guard<std::mutex> lock(mutex);pulse=true;cv.notify_all();}
void wait_end(unsigned count){
  std::unique_lock<std::mutex> lock(mutex);
  await_locked(lock,[&]{return ends>=count;});
}
void fill(unsigned color){
  auto &f=display::game_buffer();std::memset(f.colors,color,sizeof f.colors);std::memset(f.emphasis,0,sizeof f.emphasis);
}
void check_game(unsigned index,unsigned color){
  std::lock_guard<std::mutex> lock(mutex);
  assert(index<images.size());
  const auto rgb=__builtin_bswap16(presentation::token_rgb565(color));
  for(unsigned x=0;x<320;++x)for(unsigned y=0;y<240;++y)
    assert(images[index][x*240+y]==(x>=32 && x<288?rgb:0));
}
void drain(){
  if(!dma_pointer)return;
  assert(std::equal(dma_copy.begin(),dma_copy.end(),dma_pointer));
  std::copy_n(dma_pointer,240,image.begin()+(column-1)*240);dma_pointer=nullptr;
}
}
uint32_t time_us_32(){static std::atomic<uint32_t> clock{0};return clock.fetch_add(1);}
void tight_loop_contents(){if(stop.load())throw Stop{};if(pauser)pause_spinning.store(true);std::this_thread::yield();}
void tufty_service(){tight_loop_contents();}
void multicore_launch_core1(void (*entry)()){
  consumer=std::thread([entry]{try{entry();}catch(const Stop &) {}});
}
namespace tufty::memory {void sample_stack(unsigned) {}}
namespace tufty::panel {
void early_init(){} void brightness(uint8_t){} void init(){} void shutdown(){assert(!active);}
void te_sync(bool on){sync_state.enabled=on;}
SyncStatus sync_status(){return sync_state;}
EdgeStats edge_stats(){return {};}
void capture_pause(bool on){std::lock_guard<std::mutex> lock(mutex);assert(!active);if(on)++pauses;}
void wait_for_frame(){
  std::unique_lock<std::mutex> lock(mutex);assert(!active);++waits;cv.notify_all();
  cv.wait(lock,[]{return pulse || stop.load();});if(stop.load())throw Stop{};
  pulse=false;timer+=16667;
}
void begin_frame(bool sync){assert(!sync && !active);active=true;column=0;assert(!dma_pointer);}
void send_column(const uint16_t *p,unsigned count){
  assert(active && count==240 && column<320);drain();dma_pointer=p;
  std::copy_n(p,240,dma_copy.begin());++column;
}
void end_frame(){
  std::unique_lock<std::mutex> lock(mutex);assert(active && column==320);
  in_end=true;cv.notify_all();cv.wait(lock,[]{return !block_end || stop.load();});
  if(stop.load())throw Stop{};
  drain();active=false;in_end=false;images.push_back(image);++ends;cv.notify_all();
}
}
int main(){
  display::start();wait_gate(1);edge();wait_end(1);until([]{return display::disk_ready();});
  // Same menu index twice must still complete as a new request.
  display::present_disk(0);wait_gate(2);edge();wait_end(2);until([]{return display::disk_ready();});
  display::begin_game();fill(1);display::present_game();wait_gate(3);
  fill(2);display::present_game();fill(3);display::present_game();
  assert(display::stats().published==3 && display::stats().replaced==2);
  edge();wait_end(3);check_game(2,3);until([]{return display::stats().game_frames==1;});
  fill(4);display::present_game();wait_gate(4);
  {std::lock_guard<std::mutex> lock(mutex);block_end=true;}
  edge();
  {std::unique_lock<std::mutex> lock(mutex);await_locked(lock,[]{return in_end;});}
  // Producer continues while the previous frame still has DMA in flight.
  fill(5);display::present_game();fill(6);display::present_game();
  {std::lock_guard<std::mutex> lock(mutex);block_end=false;cv.notify_all();}
  wait_end(4);check_game(3,4);wait_gate(5);edge();wait_end(5);check_game(4,6);
  until([]{return display::stats().game_frames==3;});
  assert(display::stats().published==6 && display::stats().replaced==3);
  fill(7);display::present_game();wait_gate(6);
  std::thread pause_thread([]{pauser=true;display::pause(true);});
  until([]{return pause_spinning.load();});edge();pause_thread.join();
  assert(display::paused());
  {std::lock_guard<std::mutex> lock(mutex);assert(ends==5 && pauses==1 && !active);}
  fill(8);display::present_game();display::pause(false);wait_gate(7);edge();wait_end(6);check_game(5,8);
  // A UI transition during TE wait supersedes the pending game; no stale game
  // can be transmitted later over the library or disk page.
  fill(9);display::present_game();wait_gate(8);display::end_game();
  std::fill_n(display::menu_buffers[1],320*240,uint16_t(0x1234));display::present_disk(1);
  edge();wait_end(7);until([]{return display::disk_ready();});
  {std::lock_guard<std::mutex> lock(mutex);for(auto p:images.back())assert(p==uint16_t(__builtin_bswap16(uint16_t(0x1234))));}
  until([]{return display::stats().discarded==1;});
  // USB reads hold animations, while a Flash pause must still be acknowledged
  // with a pending UI request. Resume must send it once, then report ready.
  display::hold_disk(true);display::present_disk(0);
  display::pause(true);assert(display::paused() && !display::disk_ready());
  display::hold_disk(false);display::pause(false);
  wait_gate(9);edge();wait_end(8);until([]{return display::disk_ready();});
  display::te_sync(false);until([]{return !display::stats().te_enabled;});
  display::te_sync(true);until([]{return display::stats().te_enabled;});
  stop.store(true);cv.notify_all();consumer.join();
}
