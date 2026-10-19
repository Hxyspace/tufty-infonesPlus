#pragma once
#include <cstdint>

namespace power {
constexpr uint32_t kDefaultIdleSeconds=300, kMaxIdleSeconds=86400;
constexpr uint32_t kMaxSleepSeconds=4294967;

// A menu visit starts a fresh interval; time spent playing never counts.
class IdleTimer {
public:
    void activity(uint32_t now) {last_=now;}
    void menu(bool enabled,uint32_t now) {
        if(enabled!=menu_) activity(now);
        menu_=enabled;
    }
    bool due(uint32_t now,uint32_t seconds) const {
        return menu_ && seconds && seconds<=kMaxIdleSeconds && now-last_>=seconds*1000u;
    }
private:
    uint32_t last_=0;
    bool menu_=false;
};

inline bool seconds(const char *text,uint32_t limit,uint32_t &value) {
    if(!text || !*text) return false;
    uint32_t parsed=0;
    for(;*text;++text) {
        if(*text<'0' || *text>'9') return false;
        uint32_t digit=uint32_t(*text-'0');
        if(digit>limit || parsed>(limit-digit)/10u) return false;
        parsed=parsed*10u+digit;
    }
    value=parsed; return true;
}

// Requests are queued in USB callbacks and executed only at a menu boundary.
enum class Error {None, Busy, Unsaved, Backend};
struct Hooks {
    bool (*ready)();
    bool (*save)();
    Error (*enter)(uint32_t);
};
class Request {
public:
    bool queue(uint32_t duration_ms,Hooks io) {
        if(pending_) return false;
        if(!io.ready()) {error_=Error::Busy; return false;}
        duration_=duration_ms; pending_=true; error_=Error::None; return true;
    }
    void run(Hooks io) {
        if(!pending_) return;
        pending_=false;
        if(!io.ready()) {error_=Error::Busy; return;}
        if(!io.save()) {error_=Error::Unsaved; return;}
        if(!io.ready()) {error_=Error::Busy; return;}
        error_=io.enter(duration_);
        // Successful deep sleep cannot return to a partially stopped runtime.
        if(error_==Error::None) error_=Error::Backend;
    }
    bool pending() const {return pending_;}
    uint32_t duration() const {return duration_;}
    Error error() const {return error_;}
private:
    bool pending_=false;
    uint32_t duration_=0;
    Error error_=Error::None;
};
}
