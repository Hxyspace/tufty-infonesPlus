#pragma once
#include "core_api/cartridge.hpp"

namespace nes::infones_adapter {
// Single instance. ROM is borrowed until unload; its address must remain
// stable. Invalid images leave the current game intact; OOM unloads it.
// These functions and all core calls belong to the emulator thread only.
bool load(const std::uint8_t *rom,std::size_t size,const char **error=nullptr);
void unload();
// Enforced board geometry/RAM limits, independently of byte-stream inspection.
bool validate_board(const Cartridge &image,const char **error=nullptr);
}
