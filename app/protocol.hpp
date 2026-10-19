#pragma once
#include "gamepad.hpp"
#include <cstddef>
#include <cstdint>
namespace tgp {
constexpr uint8_t Version=3;
constexpr size_t PacketBytes=7+gamepad::ReportBytes;
struct Receiver {
    using Send = bool (*)(const char *, size_t);
    Send send = nullptr;
    gamepad::Receiver input;
    uint16_t id = 1;
    uint8_t sequence = 0;
    bool active = false, seen = false;
    uint32_t last_state = 0, last_alive = 0, received = 0, rejected = 0;
    char nonce[17]{};
    void byte(uint8_t value, uint32_t now);
    bool poll(uint32_t now); // True when the shared menu gesture fires.
    void reset(bool notify = false);
    void announce();
private:
    char line[80]{};
    size_t used = 0;
    bool overflow = false;
    uint8_t mode = 0; // 2: versionless v1/v2 session; 3: negotiated XTARK payload.
    void process(uint32_t now);
    bool reply(const char *kind, const char *token, const char *suffix = "");
};
}
