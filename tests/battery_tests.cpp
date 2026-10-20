#include "platform/tufty2350/battery.hpp"

#include <cassert>

int main() {
    // Reference points and limits of the native Tufty estimate curve.
    assert(battery::estimate_percent(0.0f) == 0);
    assert(battery::estimate_percent(3.2f) == 13);
    assert(battery::estimate_percent(3.3f) == 42);
    assert(battery::estimate_percent(3.5f) == 85);
    assert(battery::estimate_percent(4.2f) <= 100);
    assert(battery::estimate_percent(5.0f) == 100);
    assert(battery::estimate_percent(-1.0f) == 0);
    return 0;
}
