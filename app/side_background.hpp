#pragma once
#include "side_background_path.hpp"
namespace side_background {
// List allocation is limited to Settings. The first item is always Black.
bool open();
void close();
unsigned count();
unsigned current();
bool limited();
bool incomplete();
const char *name(unsigned index);
const char *source(unsigned index);
bool selected(unsigned index);
bool preview(unsigned index);
void apply();
presentation::GameView view();
// Core0, before begin_game publishes ownership to core1. No file access in draw.
void prepare();
void draw_preview(uint16_t *out);
}
