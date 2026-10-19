#pragma once
#include <cstdint>
#include <cstddef>
#include "bluetooth_manager.hpp"
namespace gamepad {struct Receiver;}

// All functions and BTstack callbacks run on core0, using a polling async context.
namespace ble {
enum class State {Off,Starting,Scanning,Waiting,Connecting,Discovering,Ready,Disconnecting,Backoff,Recovering,Error};
using Candidate=bluetooth::Candidate;
struct Status {
    State state;
    bool enabled,suspended,discovering;
    bluetooth::Peer peer;
    uint8_t last_error;
};
Status status(); // State reflects completion; accepted requests may still be pending.
gamepad::Receiver &input_receiver();
// Snapshot supplied by settings service; never read files in the radio/profile.
// reconnect=false updates selection policy without disrupting a healthy link.
void configure(const bluetooth::Configuration &config,bool reconnect=true);
// Deferred requests, executed by poll(); safe for console and future menu UI.
bool discover(uint32_t duration_ms=10000); // Explicitly leaves any existing link.
void stop_discovery();
size_t candidates(Candidate *out,size_t capacity);
bool connect(const bluetooth::Peer &peer); // One-shot; does not change policy.
void init();
void poll();
void shutdown();
bool command(const char *line);
uint8_t buttons();
bool home();
}
