#include "protocol.hpp"
#include "tgp_legacy.hpp"
#include <cstdio>
#include <cstring>
namespace tgp {
static int hex(char c) {
    if(c >= '0' && c <= '9') return c-'0';
    if(c >= 'a' && c <= 'f') return c-'a'+10;
    if(c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
bool Receiver::reply(const char *kind, const char *token, const char *suffix) {
    char out[64]; int n=std::snprintf(out,sizeof(out),"#TGP:%s:%s%s\n",kind,token,suffix);
    return send && n>0 && send(out,static_cast<size_t>(n));
}
void Receiver::announce() { if(send) send("#TGP:APP\n",9); }
void Receiver::reset(bool notify) {
    if(notify && active) reply("BYE",nonce);
    active=seen=false; input.release(); nonce[0]=0; used=0; overflow=false; mode=0;
}
void Receiver::byte(uint8_t value,uint32_t now) {
    if(value=='\n') {
        if(!overflow) { if(used && line[used-1]=='\r') --used; line[used]=0; process(now); }
        used=0; overflow=false;
    } else if(!overflow) {
        if(used+1<sizeof(line)) line[used++]=static_cast<char>(value);
        else { overflow=true; ++rejected; }
    }
}
void Receiver::process(uint32_t now) {
    bool legacy=used==32;
    if((legacy || (used==34 && line[32]==':' && line[33]=='3')) &&
       !std::memcmp(line,"#TGP:HELLO:",11) && line[15]==':') {
        uint16_t candidate=0;
        for(int i=11;i<15;++i) { int h=hex(line[i]); if(h<0) {++rejected; return;} candidate=(candidate<<4)|h; }
        for(int i=16;i<32;++i) if(hex(line[i])<0 || (line[i]>='A' && line[i]<='F')) {++rejected; return;}
        if(!candidate || candidate!=id) {++rejected; return;}
        char token[17]; std::memcpy(token,line+16,16); token[16]=0;
        uint8_t requested=legacy?2:3;
        if(active && !std::strcmp(nonce,token) && mode!=requested) {++rejected; return;}
        if(!reply("READY",token,legacy?"":":3")) return;
        if(!active || std::strcmp(nonce,token)) {
            input.release(); seen=false; active=true; mode=requested;
            std::memcpy(nonce,token,sizeof(nonce)); last_state=last_alive=now;
        }
        // Retrying the same HELLO cannot prolong stale keys or reset sequence.
        return;
    }
    if(active && mode==2 && used==25 && !std::memcmp(line,"#TGP:END:",9) && !std::strcmp(line+9,nonce)) {
        reset(true); return;
    }
    size_t bytes=mode==2?tgp_legacy::PacketBytes:PacketBytes;
    if(!active || used!=5+bytes*2 || std::memcmp(line,"#TGP:",5)) {++rejected; return;}
    uint8_t packet[PacketBytes];
    for(size_t i=0;i<bytes;++i) {
        int a=hex(line[5+i*2]), b=hex(line[6+i*2]);
        if(a<0 || b<0) {++rejected; return;} packet[i]=uint8_t((a<<4)|b);
    }
    if(std::memcmp(packet,"TGP",3) || (mode==3 && packet[3]!=Version) ||
       (packet[4] | uint16_t(packet[5])<<8)!=id || (seen && packet[6]==sequence)) {++rejected; return;}
    if(mode==2) {
        gamepad::ControllerState state; uint8_t seq;
        if(!tgp_legacy::decode(packet,bytes,id,state,seq)) {++rejected; return;}
        input.update(state,now);
        sequence=seq; seen=true; last_state=now; ++received; return;
    }
    const uint8_t *report=packet+7;
    if(gamepad::disconnect_report(report,gamepad::ReportBytes)) {++received; reset(true); return;}
    if(!input.receive(report,gamepad::ReportBytes,now)) {++rejected; return;}
    sequence=packet[6]; seen=true; last_state=now; ++received;
}
bool Receiver::poll(uint32_t now) {
    if(!active) return false;
    if(now-last_state>=1200) {reset(true); return false;}
    bool menu=false;
    if(now-last_state>=350) input.release();
    else menu=input.poll(now);
    if(now-last_alive>=500 && reply("ALIVE",nonce)) last_alive=now;
    return menu;
}
}
