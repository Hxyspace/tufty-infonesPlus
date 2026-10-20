#pragma once
#include <cstdint>
#define __uninitialized_psram(name)
#define __no_inline_not_in_flash_func(name) __attribute__((noinline)) name
std::uint32_t time_us_32();
void tight_loop_contents();
