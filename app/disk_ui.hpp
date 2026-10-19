#pragma once
#include <cstdint>

namespace disk_ui {
enum class State {Ready, Transferring, Unavailable, Failed, Ejected};
// Return false from service to abandon an unfinished frame when USB gets busy.
using Service=bool (*)();
bool draw(uint16_t *out,State state,uint32_t elapsed_ms,Service service=nullptr);
}
