#pragma once
#include <cstdint>
#include <filesystem>
#include "input.hpp"

namespace preview {
uint64_t micros();
void service();
void show(const uint16_t *pixels);
// Owns a host memory mapping, not an RP2350 emulator. Only State is writable.
void mount_flash(const std::filesystem::path &images);
void flush_state();
void unmount_flash();
extern bool fail_save;
void devices_init();
void devices_poll(const gamepad::ControllerState &keys);
void source(input::Source value);
input::Source source();
void battery_percent(unsigned value);
}
