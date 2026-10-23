#pragma once
#include <cstdint>

namespace power {
// Capture wake inputs before USB/Reset gesture handling.
void init();
bool reset_startup();
void activity();
void menu(bool enabled);
void idle(bool input);
bool pending();
void process(bool persistence_handled=false);
void sleep(uint32_t duration_ms=0);
uint32_t duration();
void cancel();
bool command(const char *line);
}
