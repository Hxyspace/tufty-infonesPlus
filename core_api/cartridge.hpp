#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace nes {
// Public loader admission; retained mapper implementations may exceed this.
inline constexpr std::array<std::uint16_t,9> admitted_mappers = {0,1,2,3,4,7,48,66,69};
struct Cartridge {
  std::array<std::uint8_t,16> header{};
  std::size_t prg_offset=0, prg_size=0, chr_size=0;
  std::uint16_t mapper=0;
  std::uint8_t submapper=0, timing=0;
  bool nes2=false;
};
// Allocation-free inspection. Preserves the NES 2.0 header; never truncates
// larger dimensions to the legacy core's byte-sized bank counts.
bool inspect(const std::uint8_t *data, std::size_t size, Cartridge &out,
             const char **error=nullptr);
}
