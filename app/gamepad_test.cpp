#include "gamepad_test.hpp"
#include "ui.hpp"
#include "power.hpp"
#include <algorithm>
#include <cmath>

namespace gamepad_test {
namespace {
constexpr uint32_t hold_ms=1200;
constexpr int center_x=160,trigger_width=129;
constexpr auto background=ui::rgb(8,14,21),panel=ui::rgb(15,25,35),
    idle=ui::rgb(36,51,65),edge=ui::rgb(71,90,106),ink=ui::rgb(200,213,222),
    green=ui::rgb(76,218,151);
void label(uint16_t *out,const char *text,int cx,int y,int height,uint16_t color) {
    ui::text_centered(out,text,cx-ui::width(text)/2,y,height,color);
}
void button(uint16_t *out,int x,int y,int w,int h,const char *text,bool pressed) {
    ui::rounded_box(out,x,y,w,h,5,pressed?ui::cyan:edge);
    ui::rounded_box(out,x+1,y+1,w-2,h-2,4,pressed?ui::cyan:idle);
    if(text) label(out,text,x+w/2,y,h,pressed?background:ink);
}
void face(uint16_t *out,int x,int y,const char *text,bool pressed,uint16_t color) {
    ui::circle(out,x,y,15,pressed?color:edge);
    ui::circle(out,x,y,13,pressed?color:idle);
    label(out,text,x,y-13,26,pressed?background:color);
}
void trigger(uint16_t *out,int x,int y,uint8_t value,bool pressed) {
    constexpr int width=trigger_width;
    ui::rounded_box(out,x,y,width,9,3,edge);
    ui::rounded_box(out,x+1,y+1,width-2,7,2,idle);
    const unsigned fill=(unsigned(value)*(width-2)+254)/255;
    if(fill) ui::box(out,x+1,y+1,int(fill),7,ui::cyan);
    else if(pressed) ui::box(out,x+1,y+1,width-2,7,ui::cyan);
}
void stick(uint16_t *out,int cx,int cy,uint8_t x,uint8_t y,bool clicked) {
    constexpr int radius=30;
    ui::circle(out,cx,cy,radius,clicked?green:edge);
    ui::circle(out,cx,cy,radius-2,background);
    ui::box(out,cx-radius+4,cy,2*radius-7,1,idle);
    ui::box(out,cx,cy-radius+4,1,2*radius-7,idle);
    // Both endpoints reach the circumference. Clip square-report corners to
    // the circle without changing their direction, keeping the dot visible.
    float dx=float(int(x)-128)/float(x<128?128:127);
    float dy=float(int(y)-128)/float(y<128?128:127);
    const float length2=dx*dx+dy*dy;
    const float scale=radius/(length2>1?std::sqrt(length2):1);
    const int px=int(std::lround(dx*scale)),py=int(std::lround(dy*scale));
    ui::circle(out,cx+px,cy+py,4,clicked?green:ui::cyan);
}
}
void View::open(uint32_t now) {*this={}; update(now);}
bool View::update(uint32_t now) {
    const auto pad=input::hub.gamepad();
    // Source is display-only; all buttons and analog values use one input.
    source=input::hub.active_remote();
    if(source==input::Source::None) source=input::Source::Local;
    if(epoch!=pad.epoch) {
        armed=false; holding=0; elapsed=0;
        epoch=pad.epoch;
    }
    state=pad.controller;
    if(!state.neutral()) power::activity();
    const uint8_t down=state.keys&(gamepad::KeyA|gamepad::KeyB);
    if(!armed) {armed=!down; return false;}
    // Keep one continuous button. Adding another doesn't cancel the hold,
    // but releasing it and switching buttons starts a fresh timer.
    if(!(down&holding)) {
        holding=down&uint8_t(-down); started=now; elapsed=0;
    }
    if(!holding) {elapsed=0; return false;}
    elapsed=uint16_t(std::min(hold_ms,uint32_t(now-started)));
    return elapsed==hold_ms;
}
void View::draw(uint16_t *out) const {
    std::fill(out,out+320*240,background);
    ui::text_centered(out,"Gamepad test",12,5,20,ui::white,180);
    const auto live=source==input::Source::Ble || source==input::Source::Serial;
    ui::circle(out,253,15,3,live?green:ui::orange);
    ui::text_centered(out,source==input::Source::Ble?"BLE":source==input::Source::Serial?"USB":"Local",262,5,20,live?ink:ui::orange,56);
    ui::box(out,12,27,296,1,idle);
    ui::rounded_box(out,12,31,296,179,14,panel);
    const auto keys=state.keys;
    constexpr int dpad_x=60,face_x=2*center_x-dpad_x;
    trigger(out,24,38,state.lt,keys&gamepad::KeyLT);
    trigger(out,2*center_x-24-trigger_width,38,state.rt,keys&gamepad::KeyRT);
    button(out,dpad_x-31,51,62,15,nullptr,keys&gamepad::KeyLB);
    button(out,face_x-31,51,62,15,nullptr,keys&gamepad::KeyRB);
    // Independent direction bits allow both halves of a diagonal to light.
    ui::box(out,dpad_x-11,101,22,22,idle);
    button(out,dpad_x-11,79,22,23,"^",state.dpad&gamepad::North);
    button(out,dpad_x-33,101,23,22,"<",state.dpad&gamepad::West);
    button(out,dpad_x+10,101,23,22,">",state.dpad&gamepad::East);
    button(out,dpad_x-11,122,22,23,"v",state.dpad&gamepad::South);
    face(out,face_x,87,"Y",keys&gamepad::KeyY,ui::rgb(243,199,85));
    face(out,face_x-25,112,"X",keys&gamepad::KeyX,ui::rgb(103,177,250));
    face(out,face_x+25,112,"B",keys&gamepad::KeyB,ui::rgb(245,111,109));
    face(out,face_x,137,"A",keys&gamepad::KeyA,green);
    button(out,124,120,28,10,nullptr,keys&gamepad::KeySelect);
    button(out,2*center_x-124-28,120,28,10,nullptr,keys&gamepad::KeyStart);
    stick(out,105,174,state.left_x,state.left_y,keys&gamepad::KeyL3);
    stick(out,2*center_x-105,174,state.right_x,state.right_y,keys&gamepad::KeyR3);
    if(elapsed) {
        ui::box(out,60,215,200,3,idle);
        ui::box(out,60,215,int(200u*elapsed/hold_ms),3,ui::orange);
    }
    label(out,live?"Hold A or B to return":"Connect a pad  /  Hold badge B to return",160,222,16,ink);
}
}
