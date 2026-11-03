#pragma once
#include "input.hpp"
#include <cstddef>
#include <cstdint>

namespace settings {
enum class Page {Root,Display,Control,Bluetooth,Search,Device,Allowed,AllowedDevice,Mode,Power,System,About,GameLibrary,GameOrder,LibraryView,Favorites,Palette,ControllerTest,Backgrounds,BackgroundPreview,GameDisplay,GamePosition,SideBackgrounds,SidePreview,Count};
enum class Exit {None,Library,Sleep,Disk,Bootsel,ResumeLibrary,Stay,Rebuild,Defaults,Browse};
void open();
bool active();
void update(input::Snapshot keys,bool home,uint32_t now_ms);
void draw(uint16_t *out);
// Saving and device transitions occur only after the UI frame has completed.
void after_present();
void request_exit(Exit target);
void request_save();
Exit take_exit();
bool transitioning();
// Read-only model access for host regression tests and previews.
Page page();
unsigned focus();
bool title_selected();
bool editing_id();
const char *notice();
}
