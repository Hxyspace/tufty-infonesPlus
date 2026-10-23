#pragma once
#include <cstdint>
namespace menu {
struct Timing {
    uint32_t cover_load_us=0, cover_load_max_us=0;
    uint32_t cover_hits=0, cover_misses=0, prefetches=0, prefetch_us=0, prefetch_max_us=0;
    uint32_t frames=0, prepare_us=0, prepare_max_us=0, draw_us=0, draw_max_us=0, frame_us=0, frame_max_us=0;
};
// Written/read on core0. Frame time includes display wait; prepare excludes drawing.
inline Timing timing;
void load_art();
void enter(uint32_t now_ms);
void service(uint32_t now_ms);
// UI card 0 is Settings; cards 1..count map to ROM 0..count-1.
void draw(uint16_t *out,int selected,float position,uint32_t elapsed,const char *message=nullptr);
// Select identity and prepare text; cover misses remain placeholders until presented.
void select(int index); // ROM index; -1 suspends cover loading on Settings.
// Post-presentation only; one synchronous current-cover read, no neighbor reads.
bool load_current_cover();
bool prefetch(uint32_t now_ms,bool navigation_active);
void reset_page();
void previous_page();
void next_page();
}
