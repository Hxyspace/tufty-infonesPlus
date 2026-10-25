#pragma once
#include <cstdint>
namespace flash_test {inline bool irq_disabled=false;}
inline uint32_t save_and_disable_interrupts() {
    const bool previous=flash_test::irq_disabled;
    flash_test::irq_disabled=true;
    return previous;
}
inline void restore_interrupts(uint32_t previous) {flash_test::irq_disabled=previous;}
