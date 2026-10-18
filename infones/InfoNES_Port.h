#pragma once
#include <cstddef>
#include "InfoNES_Platform.h"
namespace infones::port {
void *allocate(std::size_t bytes);
void release(void *memory);
bool has_large_memory();
bool remove_sprite_limit();
// Called per scanline, independently of presentation. Do not reenter/unload.
void service();
}
