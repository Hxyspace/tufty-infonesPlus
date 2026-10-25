#pragma once
#include <cstdint>
#include "disk_ui.hpp"

// A boot-only alternative to display::start(). Exit requires a reboot.
namespace disk_display {
void start(uint8_t brightness);
void state(disk_ui::State value);
// True only after core1 has entered its IRQ-disabled, SRAM-only loop.
bool active();
void stop();
}
