#include "bluetooth_manager.hpp"
#include <algorithm>

namespace bluetooth {
void Manager::configure(const Configuration &config,bool reconnect) {
    config_=config;
    if(reconnect) {reset(); request_=true;}
}
bool Manager::discover(uint32_t duration_ms) {
    if(!config_.enabled || duration_ms<1000 || duration_ms>60000) return false;
    duration_=duration_ms; discovery_pending_=true; request_=true; return true;
}
void Manager::stop_discovery() {
    discovery_=discovery_pending_=done_=false; request_=true;
}
bool Manager::connect(const Peer &peer) {
    if(!config_.enabled) return false;
    requested_=peer; connect_pending_=true; discovery_=discovery_pending_=false;
    request_=true; return true;
}
bool Manager::take_request() {
    if(!request_) return false;
    request_=false;
    if(discovery_pending_) {
        discovery_pending_=false; discovery_=true; started_=done_=false; count_=0;
        one_shot_=connect_pending_=false;
    }
    return true;
}
bool Manager::begin_scan(uint32_t now) {
    if(connect_pending_) {connect_pending_=false; one_shot_=true;}
    if(discovery_ && done_) return false;
    if(discovery_ && !started_) {started_=true; discovery_at_=now;}
    scan_at_=now; return true;
}
void Manager::retry() {if(!connect_pending_) one_shot_=false;}
void Manager::reset() {
    request_=discovery_=discovery_pending_=started_=done_=one_shot_=connect_pending_=false;
}
bool Manager::finish_discovery(uint32_t now) {
    if(!discovery_ || !started_ || done_ || now-discovery_at_<duration_) return false;
    done_=true; return true;
}
const Candidate *Manager::observe(const Candidate &incoming,bool complete_name) {
    size_t slot=count_;
    for(size_t i=0;i<count_;++i) if(found_[i].peer==incoming.peer) {slot=i; break;}
    if(slot==count_) {
        if(count_==Capacity) return nullptr;
        found_[count_++]={};
    }
    auto &item=found_[slot];
    item.peer=incoming.peer; item.seen_at=incoming.seen_at; item.rssi=incoming.rssi;
    // An Android peer can change profile while keeping its address. A fresh
    // service advertisement replaces the profile; name-only responses merge.
    if(incoming.hid || incoming.tgp) {
        item.hid=incoming.hid; item.tgp=incoming.tgp;
        item.controller_id=0; item.tgp_version=0;
    }
    if(incoming.controller_id) {item.controller_id=incoming.controller_id; item.tgp_version=incoming.tgp_version;}
    if(incoming.name[0] && (complete_name || !item.name[0])) {
        std::memcpy(item.name,incoming.name,sizeof(item.name)); item.name[sizeof(item.name)-1]=0;
    }
    return &item;
}
bool Manager::accepts(const Peer &peer,bool compatible,uint32_t now) const {
    if(!config_.enabled || discovery_) return false;
    if(one_shot_) return peer==requested_;
    const auto &policy=config_.policy;
    if(policy.mode==Mode::AutoCompatible) return compatible;
    int match=policy.find(peer); if(match<0) return false;
    // Give the preferred peer two seconds before accepting another allowed peer.
    return policy.preferred>=policy.count || match==policy.preferred || now-scan_at_>=2000;
}
size_t Manager::candidates(Candidate *out,size_t capacity) const {
    if(!out) return 0;
    size_t n=std::min(capacity,count_); std::copy_n(found_,n,out); return n;
}
}
