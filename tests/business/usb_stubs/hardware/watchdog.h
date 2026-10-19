#pragma once
#include <cstdint>
struct watchdog_hw_t {uint32_t scratch[8]{};};
inline watchdog_hw_t test_watchdog;
inline watchdog_hw_t *watchdog_hw=&test_watchdog;
inline bool watchdog_caused_reboot() {return false;}
[[noreturn]] void watchdog_reboot(uint32_t,uint32_t,uint32_t);
