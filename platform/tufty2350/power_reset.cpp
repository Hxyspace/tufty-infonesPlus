// Native adaptation of Pimoroni Tufty2350 modules/c/powman/powman.c:
// powman_startup(), handle_long_press(), disable_caselight_pwm().
// Copyright (c) 2026 Pimoroni Ltd. MIT; see licenses/PIMORONI-LICENSE.
#include "power_board.hpp"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/powman.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"
#include "pico/time.h"

namespace power::board {
namespace {
constexpr uint32_t kDoubleResetMs = 1000;
constexpr uint kPeak = 150, kPhase = 30, kTotal = kPeak + 3 * kPhase;
constexpr uint kDelayMs = 5, kDelaySteps = 200 / kDelayMs, kFadeSpeed = 5;
constexpr uint leds[] = {BW_LED_1, BW_LED_2, BW_LED_3, BW_LED_0};

int64_t clear_double_tap(alarm_id_t, void *) {
  powman_clear_bits(&powman_hw->chip_reset, POWMAN_CHIP_RESET_DOUBLE_TAP_BITS);
  return 0;
}

void stop_leds() {
  for (const uint pin : leds) {
    pwm_set_enabled(pwm_gpio_to_slice_num(pin), false);
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, false);
  }
}

void handle_reset_hold() {
  pwm_config config = pwm_get_default_config();
  // Include the PWM period: upstream divides only by 2048, exceeding the
  // SDK's <256 divider limit at Tufty's 250 MHz clock.
  pwm_config_set_clkdiv(&config, clock_get_hz(clk_sys) / (2048.0f * 1025.0f));
  pwm_config_set_wrap(&config, 1024);
  for (const uint pin : leds) {
    gpio_set_function(pin, GPIO_FUNC_PWM);
    pwm_init(pwm_gpio_to_slice_num(pin), &config, true);
  }
  // Official 200 ms delay, staggered rise and quick fade: about 1.64 s.
  // Only the early boot path runs here; no app/radio/Flash owner exists yet.
  for (uint step = 0; !gpio_get(BW_RESET_SW); ++step) {
    const uint progress = step < kDelaySteps ? 0 : step - kDelaySteps;
    int offset = progress >= kTotal ? int(kTotal) - int(progress - kTotal) * int(kFadeSpeed) : int(progress);
    const uint phase = progress >= kTotal ? 0 : kPhase;
    uint total_level = 0;
    for (const uint pin : leds) {
      const uint value = offset < 0 ? 0 : offset > int(kPeak) ? kPeak : uint(offset);
      const uint level = static_cast<uint>(value * 1.8f); // official LED scale
      pwm_set_gpio_level(pin, level);
      total_level += level;
      offset -= int(phase);
    }
    if (progress > 0 && total_level == 0) {
      stop_leds();
      clear_double_tap(0, nullptr);
      enter(0);
    }
    // Use the native timer wait, avoiding the alarm-pool sleep notifier.
    busy_wait_us_32(kDelayMs * 1000);
  }
  stop_leds(); // Released early: continue the ordinary boot.
}
}

bool reset_startup(bool watchdog_reset) {
  const bool flagged = powman_hw->chip_reset & POWMAN_CHIP_RESET_DOUBLE_TAP_BITS;
  clear_double_tap(0, nullptr);
  // Only a physical Reset opens the gesture window. Alarm/button wake and
  // unexpected watchdog resets must not look like a second Reset press.
  if (watchdog_reset || !(powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS)) return false;
  if (flagged) return true;

  powman_set_bits(&powman_hw->chip_reset, POWMAN_CHIP_RESET_DOUBLE_TAP_BITS);
  // Like official powman_startup(): expire asynchronously while normal boot
  // proceeds, rather than pausing all startup for the entire double-tap window.
  if (add_alarm_in_ms(kDoubleResetMs, clear_double_tap, nullptr, false) < 0) {
    clear_double_tap(0, nullptr); // Do not leave an unbounded retained flag.
  }
  if (!gpio_get(BW_RESET_SW)) handle_reset_hold();
  return false;
}
}
