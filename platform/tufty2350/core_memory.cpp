#include "InfoNES_Port.h"
#include "hardware/psram.h"
#include "arena.hpp"
#include "memory_stats.hpp"
#include <atomic>
#include <cstdlib>
#include <malloc.h>
namespace {
alignas(std::max_align_t) std::byte __uninitialized_psram("nes_heap") heap[512*1024];
tufty::Arena arena(heap,sizeof heap);
uint32_t small_fallbacks=0,allocation_failures=0;
std::atomic<uint32_t> stack_min[2]{{UINT32_MAX},{UINT32_MAX}};
}
extern "C" char __end__,__HeapLimit,__StackBottom,__StackOneBottom;
namespace tufty::memory {
void sample_stack(unsigned core){
  uintptr_t sp;
  __asm volatile("mov %0, sp" : "=r"(sp));
  const uintptr_t bottom=reinterpret_cast<uintptr_t>(core?&__StackOneBottom:&__StackBottom);
  const uint32_t margin=sp>bottom?sp-bottom:0;
  if(margin<stack_min[core].load(std::memory_order_relaxed))stack_min[core].store(margin,std::memory_order_relaxed);
}
Stats stats(){
  const auto heap_info=mallinfo();
  const auto capacity=reinterpret_cast<uintptr_t>(&__HeapLimit)-reinterpret_cast<uintptr_t>(&__end__);
  Stats s{};
  s.heap_used=heap_info.uordblks;s.heap_free=heap_info.fordblks;
  s.heap_unclaimed=capacity>size_t(heap_info.arena)?capacity-heap_info.arena:0;
  s.arena_used=arena.used_bytes();s.arena_peak=arena.peak_bytes();
  s.small_fallbacks=small_fallbacks;s.allocation_failures=allocation_failures;
  s.stack0_sample_min=stack_min[0].load();s.stack1_sample_min=stack_min[1].load();
  return s;
}
}
namespace infones::port {
void *allocate(std::size_t n){
  // Small hot CPU/PPU/APU state prefers internal SRAM. Large mapper RAM goes
  // to PSRAM; scarce internal memory can also fall back without panicking.
  if(n<32768){if(auto *p=std::malloc(n))return p;++small_fallbacks;}
  auto *p=arena.allocate(n);
  if(!p)++allocation_failures;
  return p;
}
void release(void *p){if(arena.contains(p))arena.release(p);else std::free(p);}
bool has_large_memory(){return psram_get_size()>=8*1024*1024;}
bool remove_sprite_limit(){return false;}
}
