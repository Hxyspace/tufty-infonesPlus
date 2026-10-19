#pragma once
#include <cstdint>
namespace gamepad {struct Receiver;}

namespace ble_profile {
enum class Event {Ready,Failed,RemoteClose,HeartbeatTimeout,Activity};
using Notify=void (*)(Event,uint8_t);
}
