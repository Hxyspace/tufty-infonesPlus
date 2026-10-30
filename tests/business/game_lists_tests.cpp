#include "game_lists.hpp"
#include "storage.hpp"
#include "menu.hpp"
#include "input.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string file;
static bool exists=false,write_ok=true;
static unsigned writes=0,reads=0;
static bool covers=false;
static std::vector<std::string> cover_reads;
static uint32_t generation=1;
namespace storage {
Config config;
Entry table[kMaxRoms]{}; Entry *entries=table;
size_t count=12; char error[96]{};
uint32_t generation() {return ::generation;}
FileStatus file_size(const char *,size_t &n) {n=file.size(); return exists?FileStatus::File:FileStatus::Missing;}
bool read_file(const char *path,void *out,size_t capacity,size_t &n) {
    ++reads;
    if(std::strcmp(path,"/state/game-lists.json")) {
        cover_reads.emplace_back(path); n=0; return false;
    }
    n=file.size(); if(!exists || n>capacity) return false;
    std::memcpy(out,file.data(),n); return true;
}
bool write_state(const char *,const void *data,size_t n,Progress) {
    ++writes; if(!write_ok) return false;
    exists=true; file.assign(static_cast<const char *>(data),n); return true;
}
const char *title(size_t index) {assert(index<count); return entries[index].name;}
const char *description(size_t index) {assert(index<count); return "A description with several lines.\n\nSecond page\nOne\nTwo\nThree\nFour";}
bool has_cover(size_t index) {assert(index<count); return covers;}
}
void tufty_service() {}
using game_lists::View;
static void reopen() {game_lists::close(); assert(game_lists::open());}
static void choose(View v) {game_lists::view(v); game_lists::refresh();}
static void expect(std::initializer_list<int> indices) {
    assert(game_lists::count()==indices.size()); int i=0;
    for(int index:indices) {assert(game_lists::game(i)==index); assert(game_lists::position(index)==i); ++i;}
    assert(game_lists::game(-1)==-1 && game_lists::game(i)==-1);
}
int main() {
    for(size_t i=0;i<storage::count;++i) std::snprintf(storage::entries[i].name,256,"Game%02u.nes",unsigned(i));
    assert(game_lists::open() && !reads && game_lists::session_bytes()>5120);
    assert(game_lists::toggle(2) && game_lists::toggle(0));
    choose(View::Favorites); expect({0,2});
    assert(game_lists::favorite(0) && game_lists::favorite(2) && !game_lists::favorite(1));
    assert(game_lists::pending() && !writes);
    game_lists::idle(100,true); game_lists::idle(2099,false); assert(!writes);
    game_lists::idle(2100,false); assert(writes==1 && !game_lists::pending());
    assert(game_lists::toggle(0) && game_lists::toggle(0) && !game_lists::pending());
    assert(game_lists::save() && writes==1); // Identical final order avoids a write.
    reopen(); choose(View::Favorites); expect({0,2});
    std::swap(storage::entries[0],storage::entries[4]); ++generation;
    game_lists::refresh(); expect({4,2}); // Name identity survives reordered indexes.
    assert(!game_lists::favorite(0) && game_lists::favorite(4));
    std::swap(storage::entries[0],storage::entries[4]); ++generation; game_lists::refresh();
    int selected=3; float position=3;
    choose(View::All); menu::actions(input::List,selected,position);
    assert(game_lists::view()==View::Favorites && selected==2); // Same game, different position.
    menu::actions(input::List,selected,position);
    assert(game_lists::view()==View::Recent && selected==1); // Missing in destination: first/empty card.
    assert(game_lists::game(selected-1)==-1);
    menu::actions(input::List,selected,position); assert(selected==1 && game_lists::view()==View::All);
    choose(View::Favorites); selected=2; position=2;
    menu::actions(input::Favorite,selected,position); expect({0}); assert(selected==1);
    menu::actions(input::Favorite,selected,position); expect({}); assert(selected==1);
    game_lists::discard(); expect({0,2});
    assert(game_lists::toggle(3)); const auto saved=file;
    write_ok=false; assert(!game_lists::save() && game_lists::pending() && file==saved);
    const auto attempts=writes; game_lists::idle(9000,false); game_lists::idle(12000,false); assert(writes==attempts);
    write_ok=true; assert(game_lists::save());
    alignas(8) uint8_t workspace[16*1024];
    assert(game_lists::suspend(workspace,sizeof workspace,5) && !game_lists::session_bytes());
    const auto before=writes;
    assert(game_lists::resume()); choose(View::Recent); expect({}); assert(writes==before); // Failed startup.
    assert(game_lists::suspend(workspace,sizeof workspace,5));
    game_lists::started(); assert(writes==before+1 && !game_lists::session_bytes());
    game_lists::started(); assert(writes==before+1); // Once per initialization.
    assert(game_lists::resume()); expect({5});
    for(int i=0;i<12;++i) {
        assert(game_lists::suspend(workspace,sizeof workspace,i)); game_lists::started(); assert(game_lists::resume());
    }
    expect({11,10,9,8,7,6,5,4,3,2});
    const auto no_change=writes;
    assert(game_lists::suspend(workspace,sizeof workspace,11)); game_lists::started(); assert(game_lists::resume());
    assert(writes==no_change);
    // Failed history commit remains a menu draft after the game releases SRAM.
    write_ok=false; assert(game_lists::suspend(workspace,sizeof workspace,0));
    game_lists::started(); assert(game_lists::pending()); assert(game_lists::resume());
    expect({0,11,10,9,8,7,6,5,4,3}); write_ok=true; assert(game_lists::save());
    reopen(); choose(View::Recent); expect({0,11,10,9,8,7,6,5,4,3});
    // Membership and clipped drawing must not perform file reads.
    choose(View::Favorites); menu::select(game_lists::game(0));
    std::vector<uint16_t> pixels(320*240+16,0x1234);
    const auto read_count=reads;
    for(float p:{0.0f,0.5f,1.0f,1.9f,3.0f}) menu::draw(pixels.data(),1,p,500);
    assert(reads==read_count);
    for(size_t i=320*240;i<pixels.size();++i) assert(pixels[i]==0x1234);
    covers=true; menu::load_art(); cover_reads.clear();
    menu::select(game_lists::game(1)); // Favorites [3,0,2], current Game00.
    assert(menu::load_current_cover());
    assert(cover_reads.size()==1 && cover_reads.back()=="/system/roms/Game00.565");
    assert(!menu::prefetch(100,false) && !menu::prefetch(149,false));
    assert(menu::prefetch(150,false) && cover_reads.back()=="/system/roms/Game03.565");
    assert(menu::prefetch(151,false) && cover_reads.back()=="/system/roms/Game02.565");
    assert(!menu::prefetch(152,false)); // Adjacent list positions, not catalog ±1.
    menu::next_page(); assert(menu::suspend(0));
    assert(!game_lists::session_bytes()); game_lists::started(); menu::resume();
    assert(game_lists::session_bytes());
    const auto returned_reads=reads; menu::draw(pixels.data(),2,2,500); assert(reads==returned_reads);
    for(size_t i=0;i<storage::count;++i) if(!game_lists::favorite(i) && game_lists::count()<10) assert(game_lists::toggle(i));
    assert(game_lists::count()==10 && !game_lists::toggle(11));
    assert(game_lists::save());
    // Missing favorites stay on disk, can reappear, and don't permanently fill capacity.
    std::strcpy(storage::entries[0].name,"Replacement.nes"); ++generation; game_lists::refresh();
    assert(game_lists::count()==9 && game_lists::toggle(11) && game_lists::count()==10);
    assert(game_lists::save());
    game_lists::close();
    for(const std::string &bad:{std::string("{"),std::string("{\"favorites\":[\"../a.nes\"],\"recent\":[]}"),
        std::string("{\"favorites\":[],\"favorites\":[]}"),
        std::string("{\"favorites\":[\"A.nes\\u0000evil\"],\"recent\":[]}"),
        std::string("{\"favorites\":[],\"recent\":[]}")+char(0)+"x",
        std::string("{\"favorites\":[")+std::string(1000,'[')+"0"}) {
        file=bad; exists=true; const auto count=writes;
        assert(!game_lists::open() && !game_lists::toggle(0));
        assert(game_lists::save() && writes==count && file==bad); game_lists::close();
    }
    file="{\"favorites\":[\"Game02.nes\",\"game02.NES\"],\"recent\":[]}";
    assert(game_lists::open()); choose(View::Favorites); expect({2});
    game_lists::close(); assert(!game_lists::session_bytes());
}
