#pragma once

namespace power {
// Capture wake inputs before USB/Reset gesture handling.
void init();
bool reset_startup();
void activity();
void menu(bool enabled);
void idle(bool input);
bool pending();
void process();
bool command(const char *line);
}
