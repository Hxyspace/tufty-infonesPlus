#pragma once
#include <cstdint>

namespace power::board {
// Hardware wake bits use the official Tufty channel assignments.
constexpr uint32_t kWakeVbus = 1u << 1, kWakeRtc = 1u << 2,
                   kWakeUserSwitch = 1u << 3, kWakeButtons = 1u << 4,
                   kWakeAlarm = 1u << 6;
struct WakeInfo {
  bool from_sleep = false;
  bool reset = false;
  bool watchdog = false;
  uint32_t sources = 0;
  uint32_t button_gpios = 0;
  // POWMAN scratch[0..3], independent of watchdog/ROM scratch registers.
  // A failed irreversible entry reboots; preserve why for the next status query.
  bool entry_failed = false;
  int32_t entry_rc = 0;
  uint32_t entry_state = 0, entry_pending = 0;
};
// Capture before USB double-tap detection and display initialization.
WakeInfo capture_wake();
// Run before USB/storage/apps: physical double Reset selects disk; holding
// Reset through the official LED animation enters button-wake sleep directly.
bool reset_startup(bool watchdog_reset);
bool wake_inputs_idle();
// Zero means untimed. Called only after app/display/radio/Flash owners are idle.
// Caller stops USB first. Wake reboots. A failure after irreversible hardware
// teardown deliberately resets rather than returning to the old runtime.
[[noreturn]] void enter(uint32_t duration_ms);
}
