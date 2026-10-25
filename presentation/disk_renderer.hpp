#pragma once
#include <atomic>
#include "disk_ui.hpp"

namespace disk_renderer {
struct Shared {
    std::atomic<disk_ui::State> state{disk_ui::State::Unavailable};
    std::atomic<bool> ready{false},stop{false},stopped{false};
};
static_assert(std::atomic<disk_ui::State>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
[[noreturn]] void run(Shared &shared);
}
