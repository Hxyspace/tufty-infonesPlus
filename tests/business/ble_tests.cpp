// Production BLE adapter and real BTstack HID parser, with radio/HIDS endpoints
// mocked at their public APIs. No controller or hardware is needed.
#include "ble.hpp"
#include "gamepad.hpp"
#include "tgp_legacy.hpp"
#include "controllers.hpp"
#include "protocol.hpp"
#include "storage.hpp"
#include "btstack.h"
#include "pico/stdlib.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {
uint32_t clock_ms=0;
bool disk=false,valid_map=true;
bool sync_cancel=false,sync_disconnect=false,silent_power_off=false;
bool cancel_blocks_scan=false,scan_blocked=false;
HCI_STATE radio_state=HCI_STATE_OFF;
uint32_t poll_delay=0;
unsigned inits=0,polls=0,deinits=0,scans=0,connects=0,disconnects=0,cancels=0,reads=0,activity=0;
unsigned off=0,on=0;
uint8_t connect_result=0,discovery_result=0;
uint8_t last_read_id=0,gatt_result=0;
uint16_t tgp_controller=1;
unsigned gatt_requests=0,subscriptions=0,unsubscriptions=0;
bool sync_subscription=false;
btstack_packet_handler_t gatt_handler=nullptr,notification_handler=nullptr;
btstack_packet_handler_t hci_handler=nullptr,hid_handler=nullptr,sm_handler=nullptr;
bd_addr_t connected_peer{};
bd_addr_type_t connected_type=BD_ADDR_TYPE_UNKNOWN;
// Captured from the physical XTARK controller, including its consumer report 3
// and ten-byte gamepad report 4. Keep the real descriptor as a regression case.
constexpr uint8_t map[]={
    0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x03,0x75,0x10,0x95,0x01,
    0x15,0x01,0x26,0x9c,0x02,0x19,0x01,0x2a,0x9c,0x02,0x81,0x00,0xc0,
    0x05,0x01,0x09,0x05,0xa1,0x01,0x85,0x04,0x09,0x01,0xa1,0x00,
    0x09,0x30,0x09,0x31,0x09,0x32,0x09,0x35,0x15,0x00,0x26,0xff,0x00,
    0x75,0x08,0x95,0x04,0x81,0x02,0xc0,0x09,0x39,0x15,0x00,0x25,0x07,
    0x35,0x00,0x46,0x3b,0x01,0x65,0x14,0x75,0x04,0x95,0x01,0x81,0x42,
    0x75,0x04,0x95,0x01,0x81,0x01,0x05,0x09,0x19,0x01,0x29,0x10,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x10,0x81,0x02,0x05,0x02,
    0x15,0x00,0x26,0xff,0x00,0x09,0xc4,0x09,0xc5,0x95,0x02,0x75,0x08,
    0x81,0x02,0x75,0x08,0x95,0x01,0x81,0x01,0xc0};
static_assert(sizeof(map)==122);
constexpr uint8_t keyboard_map[]={0x05,0x01,0x09,0x06,0xa1,0x01,
    0x75,0x08,0x95,0x09,0x09,0x30,0x81,0x02,0xc0};
std::vector<uint8_t> descriptor_override;
void event(std::vector<uint8_t> packet) {
    if(packet[0]==BTSTACK_EVENT_STATE) radio_state=static_cast<HCI_STATE>(packet[2]);
    assert(hci_handler); hci_handler(HCI_EVENT_PACKET,0,packet.data(),packet.size());
}
void hid(std::vector<uint8_t> packet) {
    assert(hid_handler); hid_handler(HCI_EVENT_PACKET,0,packet.data(),packet.size());
}
void advertise(uint16_t uuid=0x1812,uint8_t type=1,uint8_t address=0xaa) {
    if(scan_blocked) return;
    event({GAP_EVENT_ADVERTISING_REPORT,14,0,type,0xff,0xee,0xdd,0xcc,0xbb,address,0xb0,4,
           3,3,uint8_t(uuid),uint8_t(uuid>>8)});
}
void advertise_tgp(uint8_t version=2,uint16_t id=tgp_controller,uint8_t address=0xaa) {
    std::vector<uint8_t> packet={GAP_EVENT_ADVERTISING_REPORT,0,0,1,0xff,0xee,0xdd,0xcc,0xbb,address,0xb0,31,
        2,1,6,17,7};
    for(int i=15;i>=0;--i) packet.push_back(tgp_legacy::ServiceUuid[i]);
    const uint8_t identity[]={9,255,255,255,'T','G','P',version,uint8_t(id),uint8_t(id>>8)};
    packet.insert(packet.end(),std::begin(identity),std::end(identity)); packet[1]=packet.size()-2;
    event(packet);
}
void gatt(std::vector<uint8_t> packet) {
    assert(gatt_handler); gatt_handler(HCI_EVENT_PACKET,0,packet.data(),packet.size());
}
void query_complete(uint8_t status=0) {gatt({GATT_EVENT_QUERY_COMPLETE,7,0x40,0,0,0,0,0,status});}
void service_result() {
    std::vector<uint8_t> packet(28); packet[0]=GATT_EVENT_SERVICE_QUERY_RESULT; packet[1]=26; packet[2]=0x40;
    packet[8]=1; packet[10]=6;
    reverse_128(tgp_legacy::ServiceUuid,packet.data()+12); gatt(packet);
}
void characteristic_result(bool notify=true) {
    std::vector<uint8_t> packet(32); packet[0]=GATT_EVENT_CHARACTERISTIC_QUERY_RESULT; packet[1]=30; packet[2]=0x40;
    packet[8]=2; packet[10]=3; packet[12]=6; packet[14]=ATT_PROPERTY_READ|(notify?ATT_PROPERTY_NOTIFY:0);
    reverse_128(tgp_legacy::StateUuid,packet.data()+16); gatt(packet);
}
void tgp_sample(uint8_t buttons,uint8_t sequence,uint8_t version=2,uint16_t id=tgp_controller,uint8_t value_handle=3) {
    assert(notification_handler);
    std::vector<uint8_t> packet={GATT_EVENT_NOTIFICATION,18,0x40,0,0,0,0,0,value_handle,0,8,0,
        'T','G','P',version,uint8_t(id),uint8_t(id>>8),sequence,buttons};
    notification_handler(HCI_EVENT_PACKET,0,packet.data(),packet.size());
}
void linked(uint8_t status=0) {
    std::vector<uint8_t> e(36);
    e[0]=HCI_EVENT_META_GAP; e[1]=34; e[2]=GAP_SUBEVENT_LE_CONNECTION_COMPLETE;
    e[3]=status; e[4]=0x40; event(e);
}
void ready(uint8_t status=0) {
    hid({HCI_EVENT_GATTSERVICE_META,6,GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED,7,0,status,1,1});
}
void sample(std::initializer_list<uint8_t> data) {
    std::vector<uint8_t> packet={HCI_EVENT_GATTSERVICE_META,0,GATTSERVICE_SUBEVENT_HID_REPORT,7,0,0,4,
        uint8_t(data.size()+1),0,4};
    packet.insert(packet.end(),data); packet[1]=packet.size()-2;
    hid(packet);
}
void report(uint8_t hat,uint8_t ab=0,uint8_t menu=0) {
    sample({0x80,0x80,0x80,0x80,hat,ab,menu,0,0,0});
}
void disconnected(uint8_t reason=0x13) {event({HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x40,0,reason});}
void pairing(uint8_t status,uint8_t reason) {
    std::vector<uint8_t> packet(14);
    packet[0]=SM_EVENT_PAIRING_COMPLETE; packet[1]=12; packet[2]=0x40;
    packet[11]=status; packet[12]=reason;
    assert(sm_handler); sm_handler(HCI_EVENT_PACKET,0,packet.data(),packet.size());
}
void advance(uint32_t ms) {clock_ms+=ms; ble::poll();}
void finish_restart() {
    unsigned previous_scans=scans,previous_on=on;
    assert(off && radio_state==HCI_STATE_HALTING);
    advance(1000); assert(scans==previous_scans && on==previous_on);
    event({BTSTACK_EVENT_STATE,1,HCI_STATE_OFF}); ble::poll();
    assert(scans==previous_scans && on==previous_on);
    advance(1000); assert(on==previous_on+1 && scans==previous_scans && inits==1);
    event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); assert(scans==previous_scans+1);
}

}
absolute_time_t get_absolute_time() {return uint64_t(clock_ms)*1000;}
int cyw43_arch_init() {++inits; return 0;}
void cyw43_arch_poll() {++polls; clock_ms+=poll_delay; poll_delay=0;}
void cyw43_arch_deinit() {++deinits;}
namespace storage {Config config; bool state_ready() {return true;}}
namespace usb {tgp::Receiver pad; bool dirty=false; bool disk_mode() {return disk;}}
namespace power {void activity() {++::activity;}}
extern "C" {
void gatt_client_deserialize_service(const uint8_t *packet,int offset,gatt_client_service_t *service) {
    service->start_group_handle=little_endian_read_16(packet,offset);
    service->end_group_handle=little_endian_read_16(packet,offset+2);
    reverse_128(packet+offset+4,service->uuid128); service->uuid16=0;
}
void gatt_client_deserialize_characteristic(const uint8_t *packet,int offset,gatt_client_characteristic_t *characteristic) {
    characteristic->start_handle=little_endian_read_16(packet,offset);
    characteristic->value_handle=little_endian_read_16(packet,offset+2);
    characteristic->end_handle=little_endian_read_16(packet,offset+4);
    characteristic->properties=little_endian_read_16(packet,offset+6);
    reverse_128(packet+offset+8,characteristic->uuid128); characteristic->uuid16=0;
}
uint8_t gatt_client_discover_primary_services_by_uuid128(btstack_packet_handler_t callback,hci_con_handle_t handle,const uint8_t *uuid) {
    assert(handle==0x40 && !std::memcmp(uuid,tgp_legacy::ServiceUuid,16));
    ++gatt_requests; gatt_handler=callback; return gatt_result;
}
uint8_t gatt_client_discover_characteristics_for_service_by_uuid128(btstack_packet_handler_t callback,hci_con_handle_t handle,
    gatt_client_service_t *service,const uint8_t *uuid) {
    assert(handle==0x40 && service->start_group_handle==1 && !std::memcmp(uuid,tgp_legacy::StateUuid,16));
    ++gatt_requests; gatt_handler=callback; return gatt_result;
}
void gatt_client_listen_for_characteristic_value_updates(gatt_client_notification_t *,btstack_packet_handler_t callback,
    hci_con_handle_t handle,gatt_client_characteristic_t *characteristic) {
    assert(handle==0x40 && characteristic->value_handle==3); notification_handler=callback;
}
void gatt_client_stop_listening_for_characteristic_value_updates(gatt_client_notification_t *) {++unsubscriptions;}
uint8_t gatt_client_write_client_characteristic_configuration(btstack_packet_handler_t callback,hci_con_handle_t handle,
    gatt_client_characteristic_t *characteristic,uint16_t configuration) {
    assert(handle==0x40 && characteristic->value_handle==3 && configuration==1);
    ++gatt_requests; ++subscriptions; gatt_handler=callback;
    if(sync_subscription) {tgp_sample(0,0); query_complete();}
    return gatt_result;
}
void btstack_assert_failed(const char *,uint16_t) {assert(false);}
void hci_add_event_handler(btstack_packet_callback_registration_t *r) {hci_handler=r->callback;}
void sm_add_event_handler(btstack_packet_callback_registration_t *r) {sm_handler=r->callback;}
int hci_power_control(HCI_POWER_MODE mode) {
    if(mode==HCI_POWER_ON) {++on; radio_state=HCI_STATE_INITIALIZING; scan_blocked=false;}
    else {++off; radio_state=silent_power_off?HCI_STATE_OFF:HCI_STATE_HALTING;}
    return 0;
}
HCI_STATE hci_get_state() {return radio_state;}
void l2cap_init() {}
void sm_init() {}
void gatt_client_init() {}
void sm_set_io_capabilities(io_capability_t) {}
void sm_set_secure_connections_only_mode(bool) {}
void sm_set_authentication_requirements(uint8_t) {}
void sm_just_works_confirm(hci_con_handle_t) {}
void hids_host_init(uint8_t *,uint16_t) {}
void gap_set_connection_parameters(uint16_t,uint16_t,uint16_t,uint16_t,uint16_t,uint16_t,uint16_t,uint16_t) {}
void gap_set_scan_parameters(uint8_t,uint16_t,uint16_t) {}
void gap_start_scan() {++scans;}
void gap_stop_scan() {}
uint8_t gap_connect(const bd_addr_t address,bd_addr_type_t type) {
    ++connects; std::memcpy(connected_peer,address,6); connected_type=type; return connect_result;
}
uint8_t gap_connect_cancel() {
    ++cancels;
    if(cancel_blocks_scan) scan_blocked=true;
    if(sync_cancel) linked(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
    return 0;
}
uint8_t gap_disconnect(hci_con_handle_t) {
    ++disconnects;
    if(sync_disconnect) disconnected();
    return 0;
}
uint8_t hids_host_connect(hci_con_handle_t,btstack_packet_handler_t callback,hid_protocol_mode_t,uint16_t *cid) {
    hid_handler=callback; *cid=7; return discovery_result;
}
uint8_t hids_host_send_get_report(uint16_t,uint8_t id,hid_report_type_t) {++reads; last_read_id=id; return 0;}
const uint8_t *hids_host_descriptor_storage_get_descriptor_data(uint16_t,uint8_t) {
    return !valid_map?keyboard_map:descriptor_override.empty()?map:descriptor_override.data();
}
uint16_t hids_host_descriptor_storage_get_descriptor_len(uint16_t,uint8_t) {
    return !valid_map?sizeof(keyboard_map):descriptor_override.empty()?sizeof(map):descriptor_override.size();
}
}
int main(int argc,char **argv) {
    if(argc==2) {
        ble::init(); assert(inits==1 && on==1);
        if(!std::strncmp(argv[1],"tgp_",4)) {
            bool v1=!std::strcmp(argv[1],"tgp_v1"),handover=!std::strcmp(argv[1],"tgp_handover");
            bool no_heartbeat=!std::strcmp(argv[1],"tgp_no_heartbeat");
            if(!std::strcmp(argv[1],"tgp_id")) {
                tgp_controller=0x1234; controllers::set_controller_id(tgp_controller);
                assert(storage::config.controller==tgp_controller && usb::pad.id==tgp_controller && usb::dirty);
                ble::poll(); // Apply the deferred settings before radio events arrive.
            }
            if(handover) {
                controllers::init();
                uint8_t neutral[]={128,128,128,128,255,0,0,0,0,0};
                usb::pad.input.receive(neutral,10);
                assert(input::hub.active_remote()==input::Source::Serial);
            }
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING});
            advertise_tgp(3); advertise_tgp(2,0); advertise_tgp(2,2); assert(!connects);
            advertise_tgp(v1?1:2,tgp_controller); assert(connects==1);
            linked(); assert(!gatt_requests); // GATT work is deferred out of HCI callback.
            ble::poll(); assert(gatt_requests==1);
            if(!std::strcmp(argv[1],"tgp_discovery_failure")) {
                query_complete(); assert(disconnects==1 && !subscriptions);
                ble::shutdown(); return 0;
            }
            service_result(); query_complete(); assert(gatt_requests==1);
            if(!std::strcmp(argv[1],"tgp_request_failure")) gatt_result=ERROR_CODE_COMMAND_DISALLOWED;
            ble::poll(); assert(gatt_requests==2);
            if(gatt_result) {assert(disconnects==1); ble::shutdown(); return 0;}
            characteristic_result(); query_complete(); assert(!subscriptions);
            sync_subscription=!std::strcmp(argv[1],"tgp_sync");
            ble::poll(); assert(subscriptions==1 && gatt_requests==3);
            if(!std::strcmp(argv[1],"tgp_subscribe_failure")) {
                query_complete(ATT_ERROR_WRITE_NOT_PERMITTED); assert(disconnects==1 && unsubscriptions==1);
                ble::shutdown(); return 0;
            }
            if(!sync_subscription) {
                if(!no_heartbeat) tgp_sample(0,0,v1?1:2);
                query_complete();
            }
            assert(ble::status().state==ble::State::Ready);
            if(no_heartbeat) {
                assert(!ble::input_receiver().valid);
                advance(349); assert(!disconnects);
                advance(1); assert(disconnects==1 && unsubscriptions==1);
                assert(ble::status().state==ble::State::Disconnecting);
                tgp_sample(0x18,0); assert(!ble::buttons()); // Late notification is ignored.
                disconnected(); advance(1000); advertise_tgp(); assert(connects==2);
                assert(!off); ble::shutdown(); return 0;
            }
            assert(ble::input_receiver().valid && !ble::buttons());
            tgp_sample(0x18,1,v1?1:2); assert(ble::buttons()==(gamepad::Right|gamepad::A));
            tgp_sample(0,1,v1?1:2); assert(ble::buttons()); // Duplicate does not release or refresh.
            tgp_sample(0,2,3); tgp_sample(0,2,2,2); tgp_sample(0,2,2,tgp_controller,4); assert(ble::buttons());
            if(handover) {
                assert(input::hub.active_remote()==input::Source::Serial && !input::hub.nes());
                usb::pad.input.release(); assert(input::hub.active_remote()==input::Source::Ble && !input::hub.nes());
                tgp_sample(0,2); tgp_sample(0x18,3);
                assert(input::hub.nes()==(gamepad::Right|gamepad::A));
            }
            uint8_t seq=handover?4:2;
            // A quiet user's neutral heartbeats keep the connection alive.
            for(unsigned i=0;i<10;++i) {
                advance(120); assert(!disconnects);
                tgp_sample(0,seq++,v1?1:2);
            }
            tgp_sample(0xe0,seq++,v1?1:2);
            assert(ble::buttons()==(gamepad::B|gamepad::Select|gamepad::Start));
            advance(300);
            tgp_sample(0,seq-1,v1?1:2); // Duplicate, bad version/ID/handle cannot keep the link alive.
            tgp_sample(0,seq,3); tgp_sample(0,seq,2,2); tgp_sample(0,seq,2,tgp_controller,4);
            advance(49); assert(ble::buttons() && !disconnects);
            sync_disconnect=!std::strcmp(argv[1],"tgp_sync_disconnect");
            advance(1); assert(!ble::buttons() && !ble::input_receiver().valid);
            assert(disconnects==1 && unsubscriptions==1 && !off);
            assert(ble::status().state==(sync_disconnect?ble::State::Backoff:ble::State::Disconnecting));
            tgp_sample(0x18,seq); assert(!ble::buttons());
            ble::poll(); assert(disconnects==1 && unsubscriptions==1); // Timeout fires only once.
            if(!sync_disconnect) disconnected();
            sync_disconnect=false;
            advance(1000); advertise_tgp(v1?1:2,tgp_controller); assert(connects==2);
            linked(); ble::poll(); service_result(); query_complete();
            ble::poll(); characteristic_result(); query_complete(); ble::poll();
            if(!sync_subscription) {tgp_sample(0,0,v1?1:2); query_complete();}
            tgp_sample(0xe0,1,v1?1:2);
            assert(ble::status().state==ble::State::Ready && ble::buttons()==(gamepad::B|gamepad::Select|gamepad::Start));
            disconnected(); assert(!ble::buttons() && unsubscriptions==2);
            advance(1000); advertise(); linked(); ready(); report(255); report(2,1);
            assert(ble::buttons()==(gamepad::Right|gamepad::A)); // HID still works after TGP.
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"discovery")) {
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING});
            assert(ble::discover(1000)); ble::poll();
            advertise(); advertise(); advertise(0x180f,0,0xbb);
            assert(!connects);
            ble::Candidate list[16]; assert(ble::candidates(list,16)==2);
            assert(list[0].hid && !list[1].hid && list[0].peer.random && list[0].rssi==-80);
            advance(1000); assert(ble::status().state==ble::State::Waiting);
            advertise(); assert(!connects); // Discovery completion never auto-selects a device.
            assert(ble::connect(list[1].peer)); ble::poll();
            advertise(); assert(!connects); advertise(0x180f,0,0xbb); assert(connects==1);
            linked(); ready();
            report(255); assert(ble::status().state==ble::State::Ready);
            // A new selection survives the old link's asynchronous disconnection.
            assert(ble::connect(list[0].peer)); ble::poll(); assert(disconnects==1);
            disconnected(); advance(1000); advertise(0x180f,0,0xbb); assert(connects==1);
            advertise(); assert(connects==2); linked(); ready();
            assert(ble::discover(1000)); ble::poll(); assert(disconnects==2);
            disconnected(); advance(1000); advertise(); assert(connects==2);
            advance(1000); assert(ble::status().state==ble::State::Waiting);
            ble::stop_discovery(); ble::poll(); advertise(); assert(connects==3);
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"pending_link_loss")) {
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING});
            for(unsigned attempt=0;attempt<2;++attempt) {
                advertise(0x1812,0); assert(connects==attempt+1); linked();
                // GATT reports the link loss before our HCI handler receives
                // its underlying 0x3e event. This is not an incompatible HID.
                ready(ATT_ERROR_HCI_DISCONNECT_RECEIVED);
                assert(!disconnects && ble::status().state==ble::State::Discovering);
                disconnected(ERROR_CODE_CONNECTION_FAILED_TO_BE_ESTABLISHED);
                assert(ble::status().last_error==ERROR_CODE_CONNECTION_FAILED_TO_BE_ESTABLISHED);
                advance(999); assert(ble::status().state==ble::State::Backoff);
                advance(1); assert(ble::status().state==ble::State::Scanning);
            }
            advertise(0x1812,0); assert(connects==3); linked(); ready();
            report(255); report(2,1);
            assert(ble::status().state==ble::State::Ready && ble::buttons()==(gamepad::Right|gamepad::A));
            assert(!off && !disconnects); // No extra disconnect or radio restart.
            disconnected(); advance(1000);
            advertise(0x1812,0); linked(); ready(ERROR_CODE_UNSPECIFIED_ERROR);
            assert(!disconnects); ble::poll(); assert(disconnects==1);
            disconnected(); advance(1000);
            unsigned before=connects; advertise(0x1812,0); assert(connects==before);
            advance(29000); advertise(0x1812,0); assert(connects==before+1);
            // A real profile failure with no HCI link loss keeps its cooldown.
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"selection")) {
            bluetooth::Peer a,b;
            assert(bluetooth::address("AA:BB:CC:DD:EE:FF",a.address)); a.random=true;
            b=a; b.address[0]=0xbb;
            controllers::whitelist_add(a,true); controllers::whitelist_add(b);
            controllers::set_mode(bluetooth::Mode::WhitelistOnly);
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); ble::poll();
            advertise(0x180f,1,0xbb); assert(!connects); // Preferred peer has a scan window.
            advertise(0x180f,1,0xcc); assert(!connects); // Unknown device cannot connect.
            advertise(0x180f); assert(connects==1); // Known address bypasses UUID filter.
            linked(); ready(); disconnected(); advance(1000);
            advertise(0x180f,1,0xbb); assert(connects==1);
            advance(2000); advertise(0x180f,1,0xbb); assert(connects==2); linked(); ready();
            assert(controllers::whitelist_remove(b)); ble::poll(); disconnected(); advance(1000);
            advertise(0x1812,1,0xbb); assert(connects==2);
            controllers::whitelist_clear(); ble::poll(); advertise(); assert(connects==2);
            assert(storage::config.ble.mode==bluetooth::Mode::WhitelistOnly);
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"controls")) {
            controllers::init();
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); advertise(); linked(); ready();
            report(255); report(2,1); assert(input::hub.nes()==(gamepad::Right|gamepad::A));
            uint8_t neutral[]={128,128,128,128,255,0,0,0,0,0};
            usb::pad.input.receive(neutral,10); assert(input::hub.active_remote()==input::Source::Ble);
            controllers::set_enabled(input::Source::Ble,false); ble::poll();
            assert(!input::hub.nes() && input::hub.active_remote()==input::Source::Serial);
            disconnected(); ble::poll();
            assert(controllers::command("serial off") && !storage::config.serial_enabled);
            assert(input::hub.active_remote()==input::Source::None);
            controllers::command("serial on"); assert(storage::config.serial_enabled && !usb::pad.active);
            assert(controllers::command("ble allow AA:BB:CC:DD:EE:FF public"));
            controllers::command("ble prefer AA:BB:CC:DD:EE:FF random");
            assert(storage::config.ble.count==2 && storage::config.ble.preferred==1);
            controllers::command("ble forget AA:BB:CC:DD:EE:FF public");
            assert(storage::config.ble.count==1 && storage::config.ble.preferred==0);
            controllers::command("ble mode whitelist");
            assert(storage::config.ble.mode==bluetooth::Mode::WhitelistOnly);
            controllers::command("ble whitelist"); controllers::command("pad status");
            assert(!ble::discover() && !ble::connect(storage::config.ble.peers[0]));
            controllers::command("ble scan"); assert(storage::config.ble_enabled);
            assert(input::hub.status(input::Source::Ble).enabled);
            controllers::command("ble whitelist clear"); assert(!storage::config.ble.count);
            assert(usb::dirty && !controllers::command("help"));
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"left_stick")) {
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); advertise(); linked(); ready();
            sample({80,128,128,128,255,1,0,0,0,0}); assert(ble::buttons()==(gamepad::Left|gamepad::A));
            sample({90,128,128,128,255,0,0,0,0,0}); assert(ble::buttons()==gamepad::Left);
            sample({96,128,128,128,255,0,0,0,0,0}); assert(!ble::buttons());
            sample({95,128,128,128,255,0,0,0,0,0}); assert(!ble::buttons());
            sample({176,128,128,128,255,0,0,0,0,0}); assert(ble::buttons()==gamepad::Right);
            sample({165,128,128,128,255,0,0,0,0,0}); assert(ble::buttons()==gamepad::Right);
            sample({160,128,128,128,255,0,0,0,0,0}); assert(!ble::buttons());
            sample({0,0,128,128,255,0,0,0,0,0}); assert(ble::buttons()==(gamepad::Left|gamepad::Up));
            sample({0,0,128,128,4,0,0,0,0,0}); assert(ble::buttons()==gamepad::Down); // D-pad wins.
            sample({128,128,0,255,255,0,0,0,0,0}); assert(!ble::buttons()); // Right stick is unassigned.
            sample({80,128,128,128,255,0,0,0,0,0}); assert(ble::buttons()==gamepad::Left);
            disconnected(); advance(1000); advertise(); linked(); ready();
            sample({90,128,128,128,255,0,0,0,0,0}); assert(!ble::buttons()); // No stale hysteresis.
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"home_combo")) {
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); advertise(); linked(); ready();
            report(255,0,0x60); advance(1499); assert(!ble::home() && !ble::buttons());
            report(255,0,0x60); // Repeated reports must not restart the hold timer.
            advance(1); assert(ble::home() && !ble::buttons());
            unsigned fired_activity=activity; advance(10000); assert(activity==fired_activity);
            report(255,1,0x20); assert(ble::home() && !ble::buttons()); // Release both to clear latch.
            report(255); assert(!ble::home()); report(255,1); assert(ble::buttons()==gamepad::A);
            report(255,0,0x60); advance(1000); report(255); advance(1000); assert(!ble::home());
            report(255,0,0x20); advance(2000); assert(!ble::home()); // One click is insufficient.
            report(0,0,0x60); advance(2000); assert(!ble::home()); // D-pad pressed.
            report(255,1,0x60); advance(2000); assert(!ble::home()); // Another button pressed.
            sample({0,128,128,128,255,0,0x60,0,0,0}); advance(2000); assert(!ble::home());
            sample({128,128,255,128,255,0,0x60,0,0,0}); advance(2000); assert(!ble::home());
            sample({128,128,128,128,255,0,0x60,0,1,0}); advance(2000); assert(!ble::home());
            report(255,0,0x60); advance(1000);
            hid({HCI_EVENT_GATTSERVICE_META,5,GATTSERVICE_SUBEVENT_HID_REPORT,7,0,0,4});
            advance(1000); assert(!ble::home()); // Malformed input cancels the gesture.
            report(255,0,0x60); disconnected(); advance(2000); assert(!ble::home());
            advertise(); linked(); ready();
            clock_ms=0xffffff00u; report(255,0,0x60); advance(1500); assert(ble::home());
            report(255); assert(!ble::home()); report(255,0,0x10); assert(ble::home()); // Android Home.
            assert(!disconnects && !off); ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"remote_close")) {
            assert(gamepad::disconnect_report(gamepad::DisconnectReport,10));
            assert(!gamepad::disconnect_report(gamepad::DisconnectReport,9));
            uint8_t near[10]; std::memcpy(near,gamepad::DisconnectReport,10); near[9]=0;
            assert(!gamepad::disconnect_report(near,10));
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); advertise(); linked(); ready();
            report(2,1,0x10); assert(ble::buttons() && ble::home());
            sample({0x54,0x47,0x50,0x01,0x42,0x59,0x45,0,0,0xa5});
            assert(!ble::buttons() && !ble::home() && !disconnects); // Deferred outside callback.
            report(0,1); assert(!ble::buttons()); // Drop already queued input after the close request.
            ble::poll(); assert(disconnects==1 && !off && storage::config.ble_enabled && !usb::dirty);
            disconnected(); advance(1000); assert(scans==2 && on==1);
            advertise(); linked(); ready(); report(2,1); assert(ble::buttons()==(gamepad::Right|gamepad::A));
            sample({0x54,0x47,0x50,0x01,0x42,0x59,0x45,0,0}); // Truncated control packet cannot close.
            ble::poll(); assert(disconnects==1);
            sync_disconnect=true;
            sample({0x54,0x47,0x50,0x01,0x42,0x59,0x45,0,0,0xa5});
            ble::poll(); assert(disconnects==2 && !ble::buttons());
            advance(1000); assert(scans==3 && !off); // Synchronous link completion is safe too.
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"physical_layout")) {
            assert(btstack_hid_get_report_size_for_id(4,HID_REPORT_TYPE_INPUT,map,sizeof(map))==10);
            assert(btstack_hid_get_report_size_for_id(3,HID_REPORT_TYPE_INPUT,map,sizeof(map))==2);
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); advertise(0x1812,0); linked(); ready();
            assert(reads==1 && last_read_id==4 && !disconnects);
            report(0xa2,1,0x10); // High hat nibble is padding; low nibble means right.
            assert(ble::buttons()==(gamepad::Right|gamepad::A) && ble::home());
            // Consumer-control report 3 must neither become keys nor release them.
            hid({HCI_EVENT_GATTSERVICE_META,10,GATTSERVICE_SUBEVENT_HID_REPORT,7,0,0,3,3,0,3,0,0});
            assert(ble::buttons()==(gamepad::Right|gamepad::A) && ble::home());
            // The old Android nine-byte report no longer matches this protocol.
            hid({HCI_EVENT_GATTSERVICE_META,17,GATTSERVICE_SUBEVENT_HID_REPORT,7,0,0,4,10,0,
                 4,128,128,128,128,2,1,0,0,0});
            assert(!ble::buttons() && !ble::home());
            for(unsigned null:{8,15,255}) {report(null); assert(!ble::buttons());}
            report(0,2,0x0c); assert(ble::buttons()==(gamepad::Up|gamepad::B|gamepad::Select|gamepad::Start));
            disconnected(); advance(1000);
            // A descriptor with shifted buttons has the same rounded byte size
            // and hat offset, but cannot safely use the fixed XTARK byte offsets.
            descriptor_override.assign(std::begin(map),std::end(map));
            const uint8_t padding[]={0x75,0x04,0x95,0x01,0x81,0x01};
            auto pos=std::search(descriptor_override.begin(),descriptor_override.end(),std::begin(padding),std::end(padding));
            assert(pos!=descriptor_override.end()); pos[1]=3;
            advertise(); linked(); ready(); assert(disconnects==1 && reads==1);
            disconnected(); advance(30000);
            // Axes must also match before exposing the complete normalized state.
            descriptor_override.assign(std::begin(map),std::end(map));
            const uint8_t axis[]={0x09,0x30,0x09,0x31};
            pos=std::search(descriptor_override.begin(),descriptor_override.end(),std::begin(axis),std::end(axis));
            assert(pos!=descriptor_override.end()); pos[1]=0x31; pos[3]=0x30;
            advertise(); linked(); ready(); assert(disconnects==2 && reads==1);
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"pairing_failure")) {
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); advertise(0x1812,0); linked();
            // GATT receives the ATT failure before the app's pairing callback.
            ready(ERROR_CODE_UNSPECIFIED_ERROR); assert(!disconnects);
            pairing(ERROR_CODE_AUTHENTICATION_FAILURE,SM_REASON_PAIRING_NOT_SUPPORTED);
            assert(disconnects==1); // Same failure must not reset the disconnect timeout.
            disconnected(); advance(1000);
            advertise(0x1812,0,0xab); linked();
            // Reversing callback order must also terminate only once.
            pairing(ERROR_CODE_AUTHENTICATION_FAILURE,SM_REASON_AUTHENTHICATION_REQUIREMENTS);
            ready(ERROR_CODE_UNSPECIFIED_ERROR); assert(disconnects==2);
            disconnected(); advance(1000);
            advertise(0x1812,0,0xac); linked(); pairing(0,0); ready(); report(2,1);
            assert(disconnects==2 && ble::buttons()==(gamepad::Right|gamepad::A));
            disconnected(); advance(1000); advertise(0x1812,0,0xad); linked();
            pairing(ERROR_CODE_UNSPECIFIED_ERROR,SM_REASON_UNSPECIFIED_REASON);
            assert(disconnects==3); // Pairing's 0x1f is a genuine failure.
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"idle_link")) {
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); advertise(); linked(); ready();
            assert(reads==1);
            report(2,1,0x10); unsigned held_activity=activity;
            // Simulated notification silence must not expire a held HID state.
            for(unsigned i=0;i<600;++i) advance(1000);
            assert(ble::buttons()==(gamepad::Right|gamepad::A) && ble::home() && activity==held_activity);
            assert(!disconnects && !off && scans==1 && reads==1);
            report(255); unsigned idle_activity=activity;
            // No app-close or service-loss probe runs after HID is READY.
            for(unsigned i=0;i<600;++i) advance(1000);
            assert(!ble::buttons() && !ble::home() && activity==idle_activity);
            assert(!disconnects && !off && scans==1 && reads==1);
            report(0,2,0x10);
            // A disconnection for another handle cannot release this controller.
            event({HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x41,0,0x13});
            assert(ble::buttons()==(gamepad::Up|gamepad::B) && ble::home());
            disconnected(); assert(!ble::buttons() && !ble::home());
            advance(1000); assert(scans==2 && !off);
            // Actual link loss still reconnects, including a new random address.
            advertise(0x1812,1,0xab); linked(); ready(); assert(reads==2);
            report(2,1); assert(ble::buttons()==(gamepad::Right|gamepad::A));
            ble::command("ble status"); ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"disconnect_timeout")) {
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); advertise(); linked(); ready();
            report(2,1,0x10);
            controllers::command("ble scan"); ble::poll();
            assert(disconnects==1 && !ble::buttons() && !ble::home());
            advance(10000); assert(off==1);
            finish_restart();
            advertise(0x1812,1,0xab); linked(); ready(); report(0,2);
            assert(ble::buttons()==(gamepad::Up|gamepad::B));
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"scan_uuid")) {
            storage::config.ble_scan_uuid=0x180f;
            ble::configure({true,0x180f,{}},false);
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING});
            advertise(); assert(!connects);
            advertise(0x180f); assert(connects==1);
            ble::command("ble trace on"); linked(); ready(); report(2,1);
            assert(ble::buttons()==(gamepad::Right|gamepad::A));
            disconnected(); advance(1000);
            controllers::command("ble target AA:BB:CC:DD:EE:FF public"); ble::poll();
            advertise(0x1812,1); assert(connects==1); // Wrong address type.
            advertise(0x1812,0,0xab); assert(connects==1); // Wrong address.
            advertise(0x1812,0); assert(connects==2); // MAC bypasses scan UUID.
            linked(); ready(); report(0,1); assert(ble::buttons()==(gamepad::Up|gamepad::A));
            ble::command("ble trace off"); ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"home")) {
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING});
            advertise(); linked(); ready();
            // Home uses the fixed function bit (Button 13) in the XTARK report.
            report(255); assert(!ble::home() && !ble::buttons() && !activity);
            report(255,0,0x10); assert(ble::home() && !ble::buttons() && activity==1);
            disconnected(); advance(1000);
            advertise(); linked(); ready();
            report(255,0,0x10); assert(ble::home() && !ble::buttons() && activity==2);
            advance(60000); assert(ble::home()); // No heartbeat required even for Home.
            report(1,3,0x1c); assert(ble::home() && ble::buttons()==(gamepad::Up|gamepad::Right|15));
            // An unrelated report must not release Home; malformed selected input must.
            hid({HCI_EVENT_GATTSERVICE_META,5,GATTSERVICE_SUBEVENT_HID_REPORT,7,0,0,2});
            assert(ble::home());
            hid({HCI_EVENT_GATTSERVICE_META,5,GATTSERVICE_SUBEVENT_HID_REPORT,7,0,0,4});
            assert(!ble::home() && !ble::buttons());
            report(255,0,0x10); report(255); assert(!ble::home());
            report(255,0,0x10);
            hid({HCI_EVENT_GATTSERVICE_META,3,GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED,7,0});
            assert(!ble::home());
            report(255,0,0x10); disconnected(); assert(!ble::home()); advance(1000);
            // A controller that sends no Home bit remains a normal eight-key pad.
            advertise(); linked(); ready(); report(255,0,0x0c);
            assert(!ble::home() && ble::buttons()==(gamepad::Select|gamepad::Start));
            // Other physical function bits (L2/R2/L3/R3) must not return to menu.
            report(255,0,0x63); assert(!ble::home() && !ble::buttons());
            disconnected(); advance(1000); advertise(); linked(); ready();
            report(255,0,0x10); controllers::command("ble off"); ble::poll(); assert(!ble::home());
            ble::shutdown(); return 0;
        }
        if(!std::strcmp(argv[1],"startup_timeout")) {
            silent_power_off=true;
            advance(10000); assert(off==1); // No WORKING event on first start.
            ble::poll(); advance(1000); assert(on==2 && inits==1);
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); assert(scans==1);
            advertise(); linked(); ready(); assert(reads==1);
        } else {
            event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING});
            if(!std::strcmp(argv[1],"request_failed")) connect_result=ERROR_CODE_COMMAND_DISALLOWED;
            advertise();
            if(!std::strcmp(argv[1],"sync_cancel")) {
                sync_cancel=true;
                // BTstack can complete cancellation inline if CREATE_CONNECTION
                // is still queued. A long service gap must not lose its retry.
                advance(59093); assert(cancels==1);
                ble::poll(); assert(off==1 && scans==1);
                finish_restart();
            } else if(!std::strcmp(argv[1],"cancel_stale_scan")) {
                // Model a controller that acknowledges cancellation but produces
                // no more advertisements until powered off/on.
                cancel_blocks_scan=true;
                advance(10000); assert(cancels==1 && scan_blocked);
                linked(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
                // A command during pending recovery must not resume scanning early.
                controllers::command("ble on"); ble::poll(); assert(off==1 && scans==1);
                unsigned previous_connects=connects; advertise(0x1812,1,0xbb);
                assert(connects==previous_connects);
                finish_restart(); assert(!scan_blocked);
            } else if(!std::strcmp(argv[1],"cancel_late_success")) {
                advance(10000); assert(cancels==1);
                // A connection may finish just as cancellation was requested.
                linked(); assert(disconnects==1 && !reads && !off);
                disconnected(); ble::poll(); assert(off==1 && scans==1);
                finish_restart();
            } else if(!std::strcmp(argv[1],"connect_failed") || !std::strcmp(argv[1],"request_failed")) {
                if(!std::strcmp(argv[1],"connect_failed")) linked(ERROR_CODE_CONNECTION_TIMEOUT);
                ble::poll(); assert(off==1 && scans==1);
                finish_restart(); connect_result=0;
            } else if(!std::strcmp(argv[1],"sync_disconnect")) {
                linked(); ready(); report(2,1); sync_disconnect=true;
                controllers::command("ble scan"); ble::poll(); assert(!ble::buttons());
                advance(1000); assert(scans==2 && !off);
            } else if(!std::strcmp(argv[1],"recovery_off")) {
                advance(10000); assert(cancels==1);
                advance(10000); assert(off==1 && radio_state==HCI_STATE_HALTING);
                controllers::command("ble off"); ble::poll();
                event({BTSTACK_EVENT_STATE,1,HCI_STATE_OFF}); ble::poll();
                advance(1000); assert(on==1); // Disabling cancels automatic restart.
                controllers::command("ble on"); ble::poll(); assert(on==2 && inits==1);
                event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); assert(scans==2);
            } else {
                assert(!std::strcmp(argv[1],"lost_cancel"));
                // Model a radio poll stall, followed by a missing cancel event.
                poll_delay=59093; ble::poll(); assert(cancels==1);
                silent_power_off=true; advance(10000); assert(off==1);
                ble::poll(); advance(1000); assert(on==2 && inits==1);
                event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); assert(scans==2);
            }
            advertise(); linked(); ready(); assert(reads);
            report(2,1); assert(ble::buttons()==(gamepad::Right|gamepad::A));
        }
        ble::command("ble status"); ble::shutdown(); return 0;
    }
    // Exact protocol lengths, diagonal hats, eight buttons and reserved bits.
    gamepad::Receiver receiver;
    uint8_t data[]={128,128,128,128,15,0,0,0,0,0};
    assert(receiver.receive(data,10) && !receiver.nes());
    constexpr uint8_t hats[]={gamepad::Up,gamepad::Up|gamepad::Right,gamepad::Right,gamepad::Down|gamepad::Right,
        gamepad::Down,gamepad::Down|gamepad::Left,gamepad::Left,gamepad::Up|gamepad::Left};
    for(unsigned h=0;h<8;++h) {
        data[4]=h; data[5]=3; data[6]=12;
        assert(receiver.receive(data,10) && receiver.nes()==(hats[h]|15));
    }
    receiver.release(); assert(!receiver.nes());
    for(unsigned length=0;length<13;++length) if(length!=10) assert(!receiver.receive(data,length));
    data[5]=data[6]=0;
    for(unsigned h=0;h<256;++h) {
        data[4]=h; data[9]=0xa5; // Padding does not affect keys or direction.
        assert(receiver.receive(data,10) && receiver.nes()==((h&15)<8?hats[h&15]:0));
    }
    assert(!receiver.receive(nullptr,10));
    uint8_t address[6];
    assert(bluetooth::address("Aa:BB:cc:DD:ee:FF",address) && address[0]==0xaa && address[5]==0xff);
    for(const char *s:{"","AA:BB:CC:DD:EE:FG","AA:BB:CC:DD:EE:FFx","AA-BB:CC:DD:EE:FF"})
        assert(!bluetooth::address(s,address));
    uint16_t uuid=0;
    assert(bluetooth::uuid16("0x1812",uuid) && uuid==0x1812);
    assert(bluetooth::uuid16("0XABcd",uuid) && uuid==0xabcd);
    for(const char *s:{"","1812","0x181","0x1812x","0xG812","0x0000"})
        assert(!bluetooth::uuid16(s,uuid));

    disk=true; ble::init(); ble::poll(); assert(!inits && !polls);
    disk=false; storage::config.ble_enabled=false;
    ble::configure({false,0x1812,{}},false); ble::init(); assert(!inits);
    controllers::command("ble on"); ble::poll(); assert(inits==1 && on==1);
    event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING}); assert(scans==1);
    advance(60000); assert(scans==1 && !off); // Healthy idle scanning never restarts the controller.
    advertise(0x180f); assert(!connects); // Ignore battery-only peripherals.
    advertise(); assert(connects==1 && connected_type==BD_ADDR_TYPE_LE_RANDOM && connected_peer[0]==0xaa);
    linked(); ready(); assert(reads==1);
    report(1,3,12); assert(ble::buttons()==(gamepad::Up|gamepad::Right|15) && activity==1);
    advance(2000); assert(ble::buttons()); // Change-only HID must retain held keys.
    report(255); assert(!ble::buttons() && activity==1); // Neutral heartbeat is not activity.
    report(2,1); assert(ble::buttons()==(gamepad::Right|gamepad::A));
    // Truncated selected report releases keys, unrelated report IDs do not.
    hid({HCI_EVENT_GATTSERVICE_META,5,GATTSERVICE_SUBEVENT_HID_REPORT,7,0,0,4}); assert(!ble::buttons());
    report(2,1);
    hid({HCI_EVENT_GATTSERVICE_META,5,GATTSERVICE_SUBEVENT_HID_REPORT,7,0,0,2}); assert(ble::buttons());
    disconnected(); assert(!ble::buttons()); advance(1000); assert(scans==2);
    // Reject incompatible Report Maps and failed HID discovery.
    advertise(); linked(); valid_map=false; ready(); assert(disconnects==1 && !ble::buttons());
    disconnected(); advance(1000); unsigned before=connects; advertise(); assert(connects==before);
    advance(30000); advertise(); assert(connects==before+1); linked(); valid_map=true;
    ready(0x0e); assert(disconnects==2); disconnected(); advance(1000);
    // Explicit target accepts only matching address and address type.
    controllers::command("ble target AA:BB:CC:DD:EE:FF public"); ble::poll();
    controllers::command("ble target AA:BB:CC:DD:EE:FF random extra");
    assert(!storage::config.ble.peers[0].random); // Invalid arguments preserve settings.
    before=connects; advertise(); advertise(0x1812,0,0xab); assert(connects==before);
    advertise(0x180f,0); assert(connects==before+1); linked(); ready(); report(0,1);
    assert(ble::buttons());
    controllers::command("ble off"); ble::poll(); assert(!ble::buttons() && disconnects==3);
    disconnected(); ble::poll(); assert(off==1);
    event({BTSTACK_EVENT_STATE,1,HCI_STATE_OFF});
    controllers::command("ble on"); ble::poll(); assert(on==2 && inits==1); // No full stack reinit.
    event({BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING});
    controllers::command("ble target auto"); ble::poll(); advertise();
    advance(10000); assert(cancels==1); linked(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
    ble::poll(); finish_restart(); advertise(); linked();
    before=disconnects; advance(10000); assert(disconnects==before+1); // Discovery timeout.
    disconnected(); advance(30000); advertise();
    controllers::command("ble off"); ble::poll(); before=scans;
    controllers::command("ble on"); ble::poll(); assert(scans==before); // Wait for connect cancellation.
    linked(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER); advance(1000); assert(scans==before+1);
    connect_result=ERROR_CODE_COMMAND_DISALLOWED; advertise(); before=scans;
    ble::poll(); finish_restart(); assert(scans==before+1); connect_result=0;
    ble::command("ble status"); assert(!ble::command("help"));
    ble::shutdown(); assert(deinits==1 && !ble::buttons());
}
