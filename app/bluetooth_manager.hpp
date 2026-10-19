#pragma once
#include "bluetooth_config.hpp"

// Device discovery and connection policy; independent of HID, BTstack and files.
namespace bluetooth {
struct Configuration {
    bool enabled=true;
    uint16_t scan_uuid=0x1812;
    Policy policy;
    uint16_t controller_id=1;
};
struct Candidate {
    Peer peer;
    char name[32]{};
    int8_t rssi=0;
    uint32_t seen_at=0;
    bool hid=false,tgp=false;
    uint16_t controller_id=0;
    uint8_t tgp_version=0;
};
class Manager {
public:
    static constexpr size_t Capacity=16;
    const Configuration &configuration() const {return config_;}
    void configure(const Configuration &config,bool reconnect=true);
    bool discover(uint32_t duration_ms);
    void stop_discovery();
    bool connect(const Peer &peer);
    bool take_request();
    bool begin_scan(uint32_t now);
    void retry();
    void reset(); // Clear temporary requests, preserving saved policy and list.
    bool finish_discovery(uint32_t now);
    bool discovering() const {return (discovery_ && !done_) || discovery_pending_;}
    // Merge advertising and scan-response metadata; the manager owns the cache.
    const Candidate *observe(const Candidate &item,bool complete_name);
    bool accepts(const Peer &peer,bool compatible,uint32_t now) const;
    size_t candidates(Candidate *out,size_t capacity) const;
    size_t count() const {return count_;}
private:
    Configuration config_;
    Candidate found_[Capacity]{};
    size_t count_=0;
    bool request_=false,discovery_=false,discovery_pending_=false,started_=false,done_=false;
    bool one_shot_=false,connect_pending_=false;
    uint32_t duration_=10000,discovery_at_=0,scan_at_=0;
    Peer requested_;
};
}
