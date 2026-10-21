#include "app/menu_layout.hpp"
#include "app/menu_assets.hpp"
#include <cassert>
#include <string>

int main() {
    using menu::layout::wrap;
    const auto *font=art::body_glyphs;
    const char *paragraph="First line.\n\nSecond paragraph.";
    auto line=wrap(paragraph,0,font);
    assert(std::string(paragraph+line.start,line.end-line.start)=="First line.");
    line=wrap(paragraph,line.next,font); assert(line.start==line.end);
    line=wrap(paragraph,line.next,font);
    assert(std::string(paragraph+line.start,line.end-line.start)=="Second paragraph.");

    for(const auto &text:{std::string(70000,'W'),std::string("A long introduction with repeated spaces   and paragraphs.\n\n")+std::string(1000,'i')}) {
        uint32_t position=0;
        while(text[position]) {
            auto current=wrap(text.c_str(),position,font);
            assert(current.next>position && current.next<=text.size());
            unsigned width=0;
            for(uint32_t at=current.start;at<current.end;++at) {
                auto glyph=font[unsigned(text[at])-32];
                assert(width+glyph.width<=menu::layout::Width);
                width+=glyph.advance; assert(width<=menu::layout::Width);
            }
            assert(current.end-current.start<menu::layout::LineBytes);
            position=current.next;
        }
    }
    // Exactly fitting words followed by an overflowing space must stay on this line.
    art::Glyph fixed[95]{};
    for(auto &glyph:fixed) {glyph.advance=10; glyph.width=10;}
    const char *at_space="aaaa bbbbbbbbbbbbbb   tail"; // 19 chars = 190 px.
    const auto boundary=wrap(at_space,0,fixed);
    assert(boundary.end==19 && boundary.next==22);
    const auto in_word=wrap("aaaa bbbbbbbbbbbbbbb tail",0,fixed);
    assert(in_word.end==4 && in_word.next==5);
    // Check bitmap overhang separately from advance, using a synthetic font.
    art::Glyph glyphs[95]{};
    for(auto &glyph:glyphs) {glyph.advance=10; glyph.width=20;}
    const auto overhang=wrap("WWWWWWWWWWWWWWWWWWWW",0,glyphs);
    assert(overhang.end==18); // The nineteenth bitmap would reach x=200.
}
