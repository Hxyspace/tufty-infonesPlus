// Native adaptation of Pimoroni Tufty2350 modules/c/powman/powman.c.
// Copyright (c) 2026 Pimoroni Ltd. MIT; see licenses/PIMORONI-LICENSE.
#include "power_board.hpp"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/powman.h"
#include "hardware/psram.h"
#include "hardware/resets.h"
#include "hardware/structs/usb.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include <initializer_list>

namespace power::board {
namespace {
constexpr uint32_t kEntryFailure = 0x50575201u; // PWR, encoding version 1.
constexpr uint action_pins[] = {BW_SWITCH_A, BW_SWITCH_B, BW_SWITCH_C,
                                BW_SWITCH_UP, BW_SWITCH_DOWN};

void init_wake_inputs() {
  // Like official setup_gpio(): configure before first use, not in a passive
  // readiness query. Freshly enabled pads/pulls need time to reach idle.
  for (const uint pin : action_pins) { gpio_init(pin); gpio_pull_up(pin); }
  for (const uint pin : {BW_SWITCH_INT, BW_RTC_ALARM, BW_SWITCH_HOME, BW_RESET_SW}) {
    gpio_init(pin);
    gpio_pull_up(pin);
  }
  gpio_init(BW_VBUS_DETECT);
  gpio_disable_pulls(BW_VBUS_DETECT);
}
}

WakeInfo capture_wake() {
  WakeInfo info;
  info.from_sleep = powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS;
  info.reset = powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS;
  info.watchdog = watchdog_caused_reboot();
  if (info.watchdog && powman_hw->scratch[0] == kEntryFailure) {
    info.entry_failed = true;
    info.entry_rc = static_cast<int32_t>(powman_hw->scratch[1]);
    info.entry_state = powman_hw->scratch[2];
    info.entry_pending = powman_hw->scratch[3];
  }
  powman_hw->scratch[0] = 0; // Consume once; do not replay on unrelated resets.
  init_wake_inputs();
  busy_wait_us_32(100);
  if (info.from_sleep) {
    info.sources = powman_hw->last_swcore_pwrup & POWMAN_LAST_SWCORE_PWRUP_BITS;
    // Like official latch_inputs(), collect early before the USB startup wait.
    info.button_gpios = ~gpio_get_all() & BW_SWITCH_MASK;
    busy_wait_us_32(5000);
    info.button_gpios |= ~gpio_get_all() & BW_SWITCH_MASK;
  }
  // Pstate wake retains POWMAN configuration. Consume sources once, without
  // clearing the USB double-tap bit (the USB boot code owns that flag).
  powman_disable_all_wakeups();
  return info;
}

bool wake_inputs_idle() {
  // Pure sampling: startup already enabled the pads and their pull-ups.
  return gpio_get(BW_SWITCH_INT) && gpio_get(BW_RTC_ALARM) &&
      gpio_get(BW_SWITCH_HOME);
}

static void configure_pins() {
  // Official Tufty low-leakage layout. PSRAM is no longer used after this
  // point. Preserve button pull-ups so the shared interrupt remains usable.
  for (uint pin = 0; pin < NUM_BANK0_GPIOS; ++pin) {
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_IN);
    gpio_set_input_enabled(pin, false);
    switch (pin) {
      case BW_PSRAM_CS: gpio_set_pulls(pin, true, false); break;
      case BW_RESET_SW: case BW_SWITCH_HOME: case BW_SW_POWER_EN:
      case BW_VBAT_SENSE: case BW_SENSE_1V1:
        gpio_disable_pulls(pin); break;
      case BW_SWITCH_A: case BW_SWITCH_B: case BW_SWITCH_C:
      case BW_SWITCH_UP: case BW_SWITCH_DOWN:
        gpio_set_pulls(pin, true, false); break;
      default: gpio_set_pulls(pin, false, true); break;
    }
  }
  gpio_set_pulls(BW_VBUS_DETECT, false, false);
  gpio_set_pulls(BW_RTC_ALARM, true, false);
  gpio_set_pulls(BW_SWITCH_INT, true, false);
  gpio_set_input_enabled(BW_VBUS_DETECT, true);
  gpio_set_input_enabled(BW_RTC_ALARM, true);
  gpio_set_input_enabled(BW_SWITCH_INT, true);
  // Restore inputs and settle *before* selecting an edge detector. Otherwise
  // an already plugged-in USB supply can look like a new VBUS rising edge.
  busy_wait_us_32(100);
  // Home/BOOT is not part of the action-key interrupt; physical Reset always
  // works. Reserve PWRUP2 as in the official firmware instead of incorrectly
  // arming RESET_SW on its high/released state.
  // Official sleep() leaves VBUS wake disabled. Retain insertion wake only
  // when USB is absent at entry; an asserted VBUS is not a wake event to arm.
  if (!gpio_get(BW_VBUS_DETECT)) powman_enable_gpio_wakeup(0, BW_VBUS_DETECT, true, true);
  powman_enable_gpio_wakeup(1, BW_RTC_ALARM, true, false);
  powman_enable_gpio_wakeup(3, BW_SWITCH_INT, true, false);
}

static void power_down_usb_phy() {
  // Same PHY power-down sequence as official powman_init(), after TinyUSB
  // has stopped. No IRQ or USB callback may run after pin/PSRAM teardown.
  reset_block_mask(RESETS_RESET_USBCTRL_BITS);
  unreset_block_mask_wait_blocking(RESETS_RESET_USBCTRL_BITS);
  usb_hw->muxing = USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS;
  usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS;
  usb_hw->phy_direct = USB_USBPHY_DIRECT_TX_PD_BITS | USB_USBPHY_DIRECT_RX_PD_BITS |
      USB_USBPHY_DIRECT_DM_PULLDN_EN_BITS | USB_USBPHY_DIRECT_DP_PULLDN_EN_BITS;
  usb_hw->phy_direct_override = USB_USBPHY_DIRECT_RX_DM_BITS | USB_USBPHY_DIRECT_RX_DP_BITS |
      USB_USBPHY_DIRECT_RX_DD_BITS | USB_USBPHY_DIRECT_OVERRIDE_TX_DIFFMODE_OVERRIDE_EN_BITS |
      USB_USBPHY_DIRECT_OVERRIDE_DM_PULLUP_OVERRIDE_EN_BITS |
      USB_USBPHY_DIRECT_OVERRIDE_TX_FSSLEW_OVERRIDE_EN_BITS |
      USB_USBPHY_DIRECT_OVERRIDE_TX_PD_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_RX_PD_OVERRIDE_EN_BITS |
      USB_USBPHY_DIRECT_OVERRIDE_TX_DM_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_TX_DP_OVERRIDE_EN_BITS |
      USB_USBPHY_DIRECT_OVERRIDE_TX_DM_OE_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_TX_DP_OE_OVERRIDE_EN_BITS |
      USB_USBPHY_DIRECT_OVERRIDE_DM_PULLDN_EN_OVERRIDE_EN_BITS |
      USB_USBPHY_DIRECT_OVERRIDE_DP_PULLDN_EN_OVERRIDE_EN_BITS |
      USB_USBPHY_DIRECT_OVERRIDE_DP_PULLUP_EN_OVERRIDE_EN_BITS |
      USB_USBPHY_DIRECT_OVERRIDE_DM_PULLUP_HISEL_OVERRIDE_EN_BITS;
}

[[noreturn]] static void __no_inline_not_in_flash_func(commit_sleep)(uint32_t duration_ms) {
  powman_power_state wake = POWMAN_POWER_STATE_NONE;
  wake = powman_power_state_with_domain_on(wake, POWMAN_POWER_DOMAIN_SWITCHED_CORE);
  wake = powman_power_state_with_domain_on(wake, POWMAN_POWER_DOMAIN_XIP_CACHE);
  bool valid = powman_configure_wakeup_state(POWMAN_POWER_STATE_NONE, wake);
  powman_hw->boot[0] = powman_hw->boot[1] = powman_hw->boot[2] = powman_hw->boot[3] = 0;
  // Keep the interval entirely in the POWMAN timer's millisecond domain. The
  // external calendar RTC and the SDK microsecond absolute_time_t are separate.
  if (duration_ms) powman_enable_alarm_wakeup_at_ms(powman_timer_get_ms() + duration_ms);
  int rc = valid ? powman_set_power_state(POWMAN_POWER_STATE_NONE) : PICO_ERROR_INVALID_STATE;
  if (rc != PICO_OK) {
    // GPIO, PSRAM and IRQ configuration is already torn down. A cold reboot is
    // the only valid fallback; never run UI/stdio from this context.
    powman_hw->scratch[1] = static_cast<uint32_t>(rc);
    powman_hw->scratch[2] = powman_hw->state;
    powman_hw->scratch[3] = powman_hw->current_pwrup_req;
    powman_hw->scratch[0] = kEntryFailure; // Publish marker after payload.
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
  }
  watchdog_disable();
  while (true) __wfi();
}

[[noreturn]] void enter(uint32_t duration_ms) {
  save_and_disable_interrupts();
  powman_hw->scratch[0] = 0;
  powman_clear_bits(&powman_hw->chip_reset, POWMAN_CHIP_RESET_DOUBLE_TAP_BITS);
  powman_disable_all_wakeups();
  set_sys_clock_48mhz();
  // As official powman_init(): a 250->48 MHz drop stretches QMI MAX_SELECT
  // past the PSRAM limit unless timings are re-derived at the lower clock.
  if (psram_is_available()) {
    psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ, PICO_DEFAULT_PSRAM_MAX_SELECT,
                           PICO_DEFAULT_PSRAM_MIN_DESELECT);
    psram_reinitialize();
  }
  configure_pins();
  power_down_usb_phy();
  powman_set_bits(&powman_hw->vreg_ctrl, POWMAN_VREG_CTRL_UNLOCK_BITS);
  powman_set_debug_power_request_ignored(true);
  powman_timer_stop();
  powman_timer_set_1khz_tick_source_lposc();
  powman_timer_set_ms(0);
  powman_timer_start();
  commit_sleep(duration_ms);
}
}
