#pragma once
#include <cstdint>
namespace tufty::storage::layout {
struct Region {
    uint32_t offset, bytes;
    constexpr bool contains(uint32_t at, uint32_t count) const {
        return at <= bytes && count <= bytes - at;
    }
};
inline constexpr Region kFirmware{0, 2 * 1024 * 1024};
inline constexpr Region kRom{2 * 1024 * 1024, 1024 * 1024};
inline constexpr Region kSystem{3 * 1024 * 1024, 12 * 1024 * 1024};
inline constexpr Region kState{15 * 1024 * 1024, 1024 * 1024};
inline constexpr uint32_t kXipBase = 0x10000000, kEraseBytes = 4096;
}
