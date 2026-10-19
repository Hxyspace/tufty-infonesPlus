// Core0 owns the poll-only radio. Waiting here cannot run background work;
// use the hardware timer, without an alarm/semaphore wake event. This follows
// tufty2350-native's poll wait adaptation, including the BLE bus wake delays.
#include "hardware/timer.h"

#if !PICO_CYW43_ARCH_POLL
#error "Radio wait adaptation requires the CYW43 polling architecture"
#endif

void __wrap_cyw43_await_background_or_timeout_us(uint32_t us) {
    busy_wait_us_32(us);
}

void __wrap_cyw43_delay_us(uint32_t us) {
    busy_wait_us_32(us);
}

void __wrap_cyw43_delay_ms(uint32_t ms) {
    busy_wait_ms(ms);
}
