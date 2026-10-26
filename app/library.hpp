#pragma once
#include "storage.hpp"

namespace library {
// Maintenance is core0-only, outside a running game. Edits stay in ROM scratch
// until Settings saves/discards them; no ROM may load with an outstanding draft.
// Progress may draw cached UI. save() commits order; its index cache can recover.
bool move_to_front(size_t index,storage::Progress progress=nullptr);
bool reset_order(storage::Progress progress=nullptr);
bool custom_order();
bool pending();
bool save();
void discard();
}
