#pragma once
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace presentation {
enum class Position : std::uint8_t {Center,Left,Right};
inline const char *position_id(Position p) {return p==Position::Left?"left":p==Position::Right?"right":"center";}
inline const char *position_name(Position p) {return p==Position::Left?"Left":p==Position::Right?"Right":"Center";}
inline bool parse_position(const char *text,Position &out) {
    for(auto p:{Position::Center,Position::Left,Position::Right})
        if(!std::strcmp(text,position_id(p))) {out=p;return true;}
    return false;
}
constexpr unsigned game_x(Position p) {return p==Position::Left?0:p==Position::Right?64:32;}
// Immutable for a game session. Artwork is RGB565, column-major, 32/64 x 240.
struct GameView {
    Position position=Position::Center;
    const std::uint16_t *pixels=nullptr;
    unsigned width=0;
    bool fps=false;
    const std::uint16_t *column(unsigned x) const {
        if(!pixels || (width!=32 && width!=64)) return nullptr;
        const unsigned side=x<game_x(position)?x:x-256;
        return pixels+(side%width)*240;
    }
};
// Owned by the display consumer. Only the tiny monochrome HUD is held in SRAM.
// Sequence deltas count produced game frames; completions count displayed ones.
class FpsMonitor {
public:
    void reset(Position position,bool enabled);
    void completed(std::uint32_t sequence,std::uint32_t now);
    void column(unsigned x,std::uint16_t *pixels) const;
    std::uint32_t nes_x10() const {return nes_;}
    std::uint32_t lcd_x10() const {return lcd_;}
private:
    void raster();
    void text(unsigned group,unsigned x,const char *value);
    std::uint64_t mask_[2][7]{};
    std::uint32_t at_=0,sequence_=0,frames_=0,nes_=0,lcd_=0;
    Position position_=Position::Center;
    bool enabled_=false,started_=false,valid_=false;
};
}
