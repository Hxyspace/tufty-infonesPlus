#include "battery.hpp"

#include "hardware/adc.h"

namespace battery {
namespace {
constexpr unsigned kAdcSamples = 10;
constexpr unsigned kBatteryAdcInput = BW_VBAT_SENSE - 40;
constexpr unsigned kReferenceAdcInput = BW_SENSE_1V1 - 40;
bool initialized = false;
Reading last;

uint16_t average(unsigned input) {
    adc_select_input(input);
    uint32_t sum = 0;
    for (unsigned i = 0; i < kAdcSamples; ++i) sum += adc_read() << 4;
    return static_cast<uint16_t>(sum / kAdcSamples);
}
} // namespace

void init() {
    if (initialized) return;
    adc_init();
    adc_gpio_init(BW_VBAT_SENSE);
    adc_gpio_init(BW_SENSE_1V1);
    initialized = true;
}

bool refresh(uint32_t now_ms) {
    init();
    Reading next{};
    next.timestamp_ms = now_ms;
    next.battery_adc = average(kBatteryAdcInput);
    next.reference_adc = average(kReferenceAdcInput);
    if (next.reference_adc != 0) {
        const float voltage = (static_cast<float>(next.battery_adc) /
                               static_cast<float>(next.reference_adc)) * 2.2f;
        next.millivolts = static_cast<uint32_t>(voltage * 1000.0f + 0.5f);
        next.percent = estimate_percent(voltage);
        next.valid = true;
    }
    last = next;
    return last.valid;
}

const Reading &reading() { return last; }

} // namespace battery
