#pragma once
#include "background_path.hpp"

namespace background {
// The bounded catalog exists only while the background chooser is open.
bool open();
void close();
unsigned count();
bool limited();
bool incomplete();
const char *path(unsigned index);
const char *name(unsigned index); // Filename including extension; caller formats the label.
const char *source(unsigned index);
bool selected(unsigned index);
unsigned current();
bool preview(unsigned index);
bool apply();
void restore();
}
