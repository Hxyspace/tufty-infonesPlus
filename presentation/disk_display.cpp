#include "disk_display.hpp"
#include "disk_renderer.hpp"
#include "panel.hpp"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <cstring>

extern "C" {
extern uint8_t __disk_image_start[],__disk_image_end[],__disk_image_load[];
extern uint8_t __disk_bss_start[],__disk_bss_end[];
}
namespace disk_display {
static disk_renderer::Shared shared;
static uint8_t brightness;
static void core() {
    tufty::panel::init();
    tufty::panel::brightness(brightness);
    disk_renderer::run(shared);
}
void start(uint8_t level) {
    // The linker overlays this image on the game frames. Neither display::start
    // nor the NES producer may run in this boot mode.
    std::memcpy(__disk_image_start,__disk_image_load,__disk_image_end-__disk_image_start);
    std::memset(__disk_bss_start,0,__disk_bss_end-__disk_bss_start);
    brightness=level;
    __dmb();
    multicore_launch_core1(core);
    // Do not poll TinyUSB until the other core acknowledges the safe loop.
    while(!shared.ready.load(std::memory_order_acquire))tight_loop_contents();
}
void state(disk_ui::State value) {shared.state.store(value,std::memory_order_release);}
bool active() {return shared.ready.load(std::memory_order_acquire);}
void stop() {
    shared.stop.store(true,std::memory_order_release);
    while(!shared.stopped.load(std::memory_order_acquire))tight_loop_contents();
}
}
