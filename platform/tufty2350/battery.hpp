#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

namespace battery {

struct Reading {
    uint16_t battery_adc = 0;
    uint16_t reference_adc = 0;
    uint32_t millivolts = 0;
    uint8_t percent = 0;
    uint32_t timestamp_ms = 0;
    bool valid = false;
};

// This is the official Tufty estimate curve. It is a UI estimate, not a
// coulomb counter or a shutdown threshold. Kept separate from ADC I/O so the
// curve can be tested on the host and reused by future UI code.
inline uint8_t estimate_percent(float voltage) {
    const double ratio = std::pow(static_cast<double>(voltage) / 3.2, 80.0);
    const double value = 123.0 - 123.0 / std::pow(1.0 + ratio, 0.165);
    const double clamped = std::max(0.0, std::min(100.0, value));
    return static_cast<uint8_t>(std::lround(clamped));
}

// ADC setup is idempotent. Initialization and reading do not perform I/O
// beyond configuring the ADC; callers explicitly request a sample with
// refresh().
void init();
bool refresh(uint32_t now_ms);
const Reading &reading();

} // namespace battery
