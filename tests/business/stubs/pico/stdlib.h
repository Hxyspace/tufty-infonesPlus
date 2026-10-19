#pragma once
#define __uninitialized_psram(name)
#define __no_inline_not_in_flash_func(name) name
#define PICO_OK 0
#include <cstdint>
using absolute_time_t=uint64_t;
absolute_time_t get_absolute_time();
inline uint32_t to_ms_since_boot(absolute_time_t value) {return value/1000u;}
