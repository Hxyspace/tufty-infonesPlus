#pragma once
#include <cstdint>

namespace disk_ui {
enum class State {Ready, Transferring, Unavailable, Failed, Ejected};
void draw(uint16_t *out,State state,uint32_t elapsed_ms);
}
