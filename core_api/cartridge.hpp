#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace nes {
// Public loader admission; retained mapper implementations may exceed this.
inline constexpr std::array<std::uint16_t,23> admitted_mappers = {0,1,2,3,4,7,8,11,15,18,23,24,25,33,48,66,69,70,71,177,206,241,268};
struct Cartridge {
  std::array<std::uint8_t,16> header{};
  std::size_t prg_offset=0, prg_size=0, chr_size=0;
  std::uint16_t mapper=0;
  std::uint8_t submapper=0, timing=0; // Resolved timing: 0 NTSC, 1 PAL, 3 Dendy.
  bool nes2=false;
};
// Allocation-free inspection. Preserves the NES 2.0 header; never truncates
// larger dimensions to the legacy core's byte-sized bank counts.
bool inspect(const std::uint8_t *data, std::size_t size, Cartridge &out,
             const char **error=nullptr);
}
