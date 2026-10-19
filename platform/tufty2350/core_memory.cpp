#include "InfoNES_Port.h"
#include "hardware/psram.h"
#include "arena.hpp"
#include <cstdlib>
namespace {
alignas(std::max_align_t) std::byte __uninitialized_psram("nes_heap") heap[512*1024];
tufty::Arena arena(heap,sizeof heap);
}
namespace infones::port {
void *allocate(std::size_t n){
  // Small hot CPU/PPU/APU state prefers internal SRAM. Large mapper RAM goes
  // to PSRAM; scarce internal memory can also fall back without panicking.
  if(n<32768)if(auto *p=std::malloc(n))return p;
  return arena.allocate(n);
}
void release(void *p){if(arena.contains(p))arena.release(p);else std::free(p);}
bool has_large_memory(){return psram_get_size()>=8*1024*1024;}
bool remove_sprite_limit(){return false;}
}
