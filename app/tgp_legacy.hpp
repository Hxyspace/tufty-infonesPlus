#pragma once
#include "gamepad.hpp"
#include <cstring>

// TGP v1/v2 wire adapter, shared by custom BLE and CDC1. No transport state.
namespace tgp_legacy {
constexpr size_t PacketBytes=8;
constexpr uint8_t ServiceUuid[]={0xc5,0x3a,0x10,0x00,0x6e,0x5d,0x4a,0x89,0x8f,0x3c,0x9e,0x5a,0x72,0x64,0xf1,0x01};
constexpr uint8_t StateUuid[]={0xc5,0x3a,0x10,0x01,0x6e,0x5d,0x4a,0x89,0x8f,0x3c,0x9e,0x5a,0x72,0x64,0xf1,0x01};
inline bool supported(uint8_t version) {return version==1 || version==2;}
inline bool identity(const uint8_t *data,size_t bytes,uint16_t &id,uint8_t &version) {
    if(!data || bytes!=8 || data[0]!=0xff || data[1]!=0xff || std::memcmp(data+2,"TGP",3) ||
       !supported(data[5])) return false;
    uint16_t value=data[6]|uint16_t(data[7])<<8;
    if(!value) return false;
    id=value; version=data[5]; return true;
}
inline bool decode(const uint8_t *data,size_t bytes,uint16_t id,
                   gamepad::ControllerState &state,uint8_t &sequence) {
    if(!data || bytes!=PacketBytes || std::memcmp(data,"TGP",3) || !supported(data[3]) ||
       !id || (data[4]|uint16_t(data[5])<<8)!=id) return false;
    uint8_t bits=data[7]; state={}; sequence=data[6];
    if(bits&1) state.dpad|=gamepad::North;
    if(bits&2) state.dpad|=gamepad::South;
    if(bits&4) state.dpad|=gamepad::West;
    if(bits&8) state.dpad|=gamepad::East;
    if(bits&16) state.keys|=gamepad::KeyA;
    if(bits&32) state.keys|=gamepad::KeyB;
    if(bits&64) state.keys|=gamepad::KeySelect;
    if(bits&128) state.keys|=gamepad::KeyStart;
    return true;
}
}
