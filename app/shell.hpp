#pragma once
#include <cstddef>
#include <cstdint>

// CDC0 shell; no terminal emulation, logging interception or storage writes.
namespace shell {
using Write=bool (*)(const char *,size_t);
using Execute=void (*)(const char *);
using Available=bool (*)();
void init(Write write,Execute execute,Available files_available);
void reset();
void prompt();
void feed(uint8_t byte);
// Nonblocking output only; safe from the USB service (no filesystem access).
void poll();
// Called only by the menu dispatcher, never by the in-game USB service.
void service();
// Cancel temporary output before launching a game or leaving the menu.
void cancel();
}
