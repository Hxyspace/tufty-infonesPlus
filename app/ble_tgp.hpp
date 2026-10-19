#pragma once
#include "ble_profile.hpp"

// Custom TGP v1/v2 GATT input. Uses the same logical receiver as HID.
namespace ble_tgp {
void init(gamepad::Receiver &input,ble_profile::Notify notify);
void connect(uint16_t handle,uint16_t controller_id);
void reset();
void poll(uint32_t now);
uint8_t version();
}
