#pragma once
#include "ble_profile.hpp"

// HID profile adapter. No device lists, selection policy or persistent settings.
namespace ble_gamepad {
using ble_profile::Event;
using ble_profile::Notify;
void init(gamepad::Receiver &input,Notify notify);
uint8_t connect(uint16_t handle);
void reset();
void trace(bool enabled);
uint8_t service();
uint8_t report();
}
