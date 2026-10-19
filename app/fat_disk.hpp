#pragma once
#include <cstdint>
#include "layout.hpp"
namespace flashfat {
using Read = bool (*)(uint32_t, void *, uint32_t);
struct Disk {
    Read read = nullptr;
    uint16_t sector_size = 0;
    bool runtime_owned = false;
    // Validate an unpartitioned FAT BPB before passing geometry to FatFs.
    bool probe();
    bool sectors(uint32_t lba, uint32_t count, void *out) const;
    bool range(uint32_t lba, uint32_t offset, uint32_t size, uint32_t &address) const;
};
extern Disk disk;
}
