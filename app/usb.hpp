#pragma once
#include <cstdint>
#include "protocol.hpp"
namespace usb {
constexpr uint16_t vid=0xcafe, runtime_pid=0x4025, disk_pid=0x4027;
extern tgp::Receiver pad;
extern bool disk_requested, rescan_requested, save_requested, dirty;
bool consume_disk_request();
bool disk_mode();
void init(bool disk);
void poll();
void transition_notice(const char *message);
void shutdown();
void acquire_disk(bool valid);
[[noreturn]] void reboot_disk();
[[noreturn]] void run_disk();
}
