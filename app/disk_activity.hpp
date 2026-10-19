#pragma once
#include <cstdint>

namespace usb {
// Group reads/writes into bursts, including a quiet interval after completion.
// Host status probes do not count as file operations.
class DiskActivity {
public:
    void begin(uint32_t now) {seen_=in_flight_=true; last_=now;}
    void complete(uint32_t now) {in_flight_=false; if(seen_) last_=now;}
    void reset() {seen_=in_flight_=false;}
    bool busy(uint32_t now) const {return seen_ && (in_flight_ || now-last_<500);}
private:
    uint32_t last_=0;
    bool seen_=false,in_flight_=false;
};
}
