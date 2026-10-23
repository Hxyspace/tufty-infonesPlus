#pragma once
#include <cstdint>
namespace art {struct Glyph;}

// Shared library/settings renderer. Uses the existing background and UI buffers.
namespace ui {
enum class Font {Body,Title};
enum class Icon {Header,Display,Control,Power,System,About,Settings,Count};
enum class BatteryStyle {Library,Settings};
constexpr int bar_height=32, status_center_y=bar_height/2;
constexpr uint16_t rgb(int r,int g,int b) {return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
constexpr uint16_t white=0xffff,cyan=rgb(0,193,222),orange=rgb(255,174,45),
    grey=rgb(192,197,203),dim=rgb(115,120,128),blue=rgb(0,43,73);
void canvas(uint16_t *out);
void status_icons(uint16_t *out,BatteryStyle style=BatteryStyle::Library);
void load_background();
// Load once at resource preparation; drawing never reads files. Reload also
// invalidates cached failures when resources are refreshed.
void load_icons();
bool icon(uint16_t *out,Icon which,int x,int y);
void service(uint32_t now_ms);
uint16_t blend(uint16_t a,uint16_t b,int alpha);
void circle(uint16_t *out,int cx,int cy,int radius,uint16_t color);
int glyph_width(const char *s,const art::Glyph *glyphs);
void glyph_text(uint16_t *out,const char *s,int x,int y,uint16_t color,
                const art::Glyph *glyphs,const uint8_t *pixels,int clip_top=0,int clip_bottom=240);
void box(uint16_t *out,int x,int y,int w,int h,uint16_t color);
void rounded_box(uint16_t *out,int x,int y,int w,int h,int radius,uint16_t color);
int width(const char *s,Font font=Font::Body);
void text(uint16_t *out,const char *s,int x,int y,uint16_t color,
          int max_width=272,Font font=Font::Body);
// Vertically center the visible ink, excluding transparent font padding.
void text_centered(uint16_t *out,const char *s,int x,int top,int height,
                   uint16_t color,int max_width=272);
}
