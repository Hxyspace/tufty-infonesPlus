#pragma once
#include <cstddef>
#include <cstdint>

namespace game_lists {
enum class View : uint8_t {All,Favorites,Recent};
constexpr size_t limit=10;
// A menu-owned SRAM session. The game uses the caller's idle UI workspace
// instead, releasing this allocation before the emulator allocates its state.
bool open();
void close();
void refresh();
View view();
void view(View value);
size_t count();
int game(int position);
int position(int game);
bool favorite(size_t game);
bool toggle(size_t game);
bool pending();
bool save();
void discard();
void idle(uint32_t now,bool busy);
// Suspend into aligned, non-ROM UI memory, borrowed until resume().
bool suspend(void *workspace,size_t bytes,int game);
void started(); // Once, after successful core initialization, before frames.
bool resume();
size_t session_bytes();
}
