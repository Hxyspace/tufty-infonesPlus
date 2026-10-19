#pragma once
#include "../../stubs/pico/stdlib.h"
#include <stdexcept>
using uint=unsigned;
#define BW_LED_0 0
#define BW_LED_1 1
#define BW_LED_2 2
#define BW_LED_3 3
#define BTN_HOME 22
uint32_t time_us_32();
void sleep_ms(uint32_t);
void gpio_put(uint,bool);
inline bool gpio_get(uint) {return true;}
void tight_loop_contents();
[[noreturn]] inline void panic(const char *) {throw std::runtime_error("panic");}
