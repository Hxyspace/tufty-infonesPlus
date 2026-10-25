// This entire object's code, constants and data are in the disk SRAM overlay.
// No callbacks, IRQs, PSRAM or locks shared with core0 are used after entry.
#include "disk_renderer.hpp"
#include "panel.hpp"
#include "hardware/sync.h"
#include "pico/stdlib.h"

namespace disk_renderer {
static uint16_t pixels[320*240];
[[noreturn]] void run(Shared &shared) {
    save_and_disable_interrupts();
    shared.ready.store(true,std::memory_order_release);
    uint32_t previous=time_us_32()-33333;
    for(;;) {
        if(shared.stop.load(std::memory_order_acquire)) {
            shared.stopped.store(true,std::memory_order_release);
            for(;;) __asm volatile("nop");
        }
        const uint32_t now=time_us_32();
        if(now-previous<33333)continue;
        disk_ui::draw(pixels,shared.state.load(std::memory_order_acquire),(now-previous)/1000);
        previous=now;
        tufty::panel::disk_frame(pixels);
    }
}
}
