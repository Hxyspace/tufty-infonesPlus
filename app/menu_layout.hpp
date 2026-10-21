#pragma once
#include <algorithm>
#include <cstdint>

namespace menu::layout {
constexpr unsigned Width=194, LinesPerPage=4, LineBytes=Width+1;
// One checkpoint per 16 pages covers even a maximum-sized all-newline index.
constexpr unsigned CheckpointLines=64, MaxCheckpoints=256*1024/CheckpointLines;
struct Line {uint32_t start,end,next;};

template<class Glyph>
Line wrap(const char *text,uint32_t position,const Glyph *glyphs) {
    while(text[position]==' ') ++position;
    const uint32_t start=position;
    uint32_t space=UINT32_MAX;
    unsigned width=0;
    while(text[position] && text[position]!='\n') {
        const unsigned c=static_cast<unsigned char>(text[position]);
        const auto &glyph=glyphs[(c>=32 && c<127?c:'?')-32];
        // Bitmap width can extend beyond its advance (e.g. an italic glyph).
        if(width+std::max(unsigned(glyph.advance),unsigned(glyph.width))>Width) break;
        width+=glyph.advance;
        if(text[position]==' ') space=position;
        ++position;
    }
    uint32_t end=position,next=position;
    if(text[position] && text[position]!=' ' && text[position]!='\n' && space!=UINT32_MAX) {end=space; next=space+1;}
    // Ensure progress even if a future font contains a glyph wider than the area.
    else if(position==start && text[position] && text[position]!='\n') end=next=position+1;
    while(text[next]==' ') ++next;
    if(text[next]=='\n') ++next;
    while(end>start && text[end-1]==' ') --end;
    return {start,end,next};
}
} // namespace menu::layout
