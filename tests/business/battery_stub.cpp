#include "platform/tufty2350/battery.hpp"

namespace battery {

Reading test_value{};
unsigned test_refreshes=0;

bool refresh(uint32_t now_ms) {
    ++test_refreshes;
    test_value.timestamp_ms = now_ms;
    return test_value.valid;
}

const Reading &reading() {
    return test_value;
}

} // namespace battery
