#pragma once
#include "input.hpp"

namespace menu {
// Shared menu-only repeat. Deadlines restart from now: never replay a backlog.
class Repeat {
    struct Timer {uint32_t deadline=0; bool active=false;};
    Timer timers[4];
    uint32_t epoch=0;
    uint8_t conflicted=0;
public:
    void reset() {for(auto &timer:timers) timer={}; conflicted=0;}
    uint8_t poll(const input::Snapshot &snapshot,uint32_t now) {
        auto keys=snapshot;
        if(epoch!=keys.epoch) {reset(); epoch=keys.epoch;}
        const uint8_t axes[]={input::Previous|input::Next,input::PagePrevious|input::PageNext};
        for(unsigned axis=0;axis<2;++axis) {
            const auto mask=axes[axis];
            if(((keys.held|keys.pressed)&mask)==mask) {
                conflicted|=mask; keys.held&=~mask; keys.pressed&=~mask;
                timers[axis*2]={}; timers[axis*2+1]={};
            } else if(conflicted&mask) {
                // A remaining held direction starts a fresh delay, not a new edge.
                keys.pressed&=~mask; conflicted&=~mask;
            }
        }
        uint8_t result=keys.pressed;
        const uint8_t bits[]={input::Previous,input::Next,input::PagePrevious,input::PageNext};
        bool repeated=false;
        for(unsigned i=0;i<4;++i) {
            auto &timer=timers[i]; const auto bit=bits[i];
            if(!(keys.held&bit)) {timer={}; continue;}
            if(!timer.active || (keys.pressed&bit) || (keys.released&bit)) {
                timer={now+400,true}; continue;
            }
            if(static_cast<int32_t>(now-timer.deadline)>=0) {
                timer.deadline=now+150;
                if(!repeated) {result|=bit; repeated=true;}
            }
        }
        return result;
    }
};
inline bool launch_requested(uint8_t pressed) {
    return (pressed&input::Confirm) && !(pressed&(input::Previous|input::Next));
}
}
