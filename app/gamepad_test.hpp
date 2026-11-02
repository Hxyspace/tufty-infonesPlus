#pragma once
#include "input.hpp"

namespace gamepad_test {
// A menu-only snapshot and hold gesture; no extra work in game input polling.
class View {
public:
    void open(uint32_t now);
    bool update(uint32_t now); // True after a fresh logical A or B hold.
    void draw(uint16_t *out) const;
private:
    gamepad::ControllerState state;
    input::Source source=input::Source::None;
    uint32_t epoch=0,started=0;
    uint16_t elapsed=0;
    uint8_t holding=0;
    bool armed=false;
};
}
