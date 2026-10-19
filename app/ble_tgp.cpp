#include "ble_tgp.hpp"
#include "tgp_legacy.hpp"
#include "pico/stdlib.h"
#include "btstack.h"
#include <cstring>

namespace ble_tgp {
namespace {
using ble_profile::Event;
enum class Stage {Idle,Service,Characteristic,Subscribe,Ready};
Stage stage=Stage::Idle;
bool pending=false,listening=false,seen=false;
uint8_t sequence=0,wire_version=0;
uint16_t controller_id=1;
uint32_t last_state=0;
hci_con_handle_t connection=HCI_CON_HANDLE_INVALID;
gatt_client_service_t service{};
gatt_client_characteristic_t characteristic{};
gatt_client_notification_t listener{};
gamepad::Receiver *receiver=nullptr;
ble_profile::Notify notify=nullptr;
void fail(uint8_t error) {
    reset(); notify(Event::Failed,error);
}
void event(uint8_t packet_type,uint16_t,uint8_t *packet,uint16_t size) {
    if(packet_type!=HCI_EVENT_PACKET || size<4 || stage==Stage::Idle ||
       little_endian_read_16(packet,2)!=connection) return;
    switch(packet[0]) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            if(stage!=Stage::Service || size<28 || service.start_group_handle) return;
            gatt_event_service_query_result_get_service(packet,&service);
            if(std::memcmp(service.uuid128,tgp_legacy::ServiceUuid,16)) service={};
            break;
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            if(stage!=Stage::Characteristic || size<32 || characteristic.value_handle) return;
            gatt_event_characteristic_query_result_get_characteristic(packet,&characteristic);
            if(std::memcmp(characteristic.uuid128,tgp_legacy::StateUuid,16)) characteristic={};
            break;
        case GATT_EVENT_QUERY_COMPLETE: {
            if(size<9 || pending) return;
            uint8_t status=gatt_event_query_complete_get_att_status(packet);
            if(status) {fail(status); return;}
            if(stage==Stage::Service) {
                if(!service.start_group_handle) {fail(ATT_ERROR_ATTRIBUTE_NOT_FOUND); return;}
                stage=Stage::Characteristic; pending=true;
            } else if(stage==Stage::Characteristic) {
                if(!characteristic.value_handle || !(characteristic.properties&ATT_PROPERTY_NOTIFY)) {
                    fail(ATT_ERROR_ATTRIBUTE_NOT_FOUND); return;
                }
                stage=Stage::Subscribe; pending=true;
            } else if(stage==Stage::Subscribe) {
                if(!seen) last_state=to_ms_since_boot(get_absolute_time());
                stage=Stage::Ready; notify(Event::Ready,0);
            }
            break;
        }
        case GATT_EVENT_NOTIFICATION: {
            // Android can send its initial neutral notification before the CCCD
            // write response. Accept it once the subscription has been issued.
            if((stage!=Stage::Ready && stage!=Stage::Subscribe) || !listening || size<12 ||
               gatt_event_notification_get_value_handle(packet)!=characteristic.value_handle) return;
            uint16_t bytes=gatt_event_notification_get_value_length(packet);
            if(size<12+bytes) {++receiver->rejected; return;}
            const uint8_t *data=gatt_event_notification_get_value(packet);
            gamepad::ControllerState state; uint8_t seq;
            if(!tgp_legacy::decode(data,bytes,controller_id,state,seq) || (seen && seq==sequence)) {
                ++receiver->rejected; return;
            }
            sequence=seq; seen=true; wire_version=data[3];
            last_state=to_ms_since_boot(get_absolute_time());
            receiver->update(state,last_state);
            if(receiver->contributing() && receiver->nes()) notify(Event::Activity,0);
            break;
        }
        default:break;
    }
}
}
void init(gamepad::Receiver &input,ble_profile::Notify callback) {receiver=&input; notify=callback;}
void connect(uint16_t handle,uint16_t id) {
    reset(); connection=handle; controller_id=id; stage=Stage::Service; pending=true;
}
void reset() {
    stage=Stage::Idle; pending=seen=false; wire_version=0;
    connection=HCI_CON_HANDLE_INVALID; service={}; characteristic={};
    if(listening) {gatt_client_stop_listening_for_characteristic_value_updates(&listener); listening=false;}
    if(receiver) receiver->release();
}
void poll(uint32_t now) {
    // Count from subscription readiness until the first valid packet, then
    // from the last valid heartbeat. HID never enters this profile's timeout.
    if(stage==Stage::Ready && now-last_state>=350) {
        reset(); notify(Event::HeartbeatTimeout,0); return;
    }
    if(!pending) return;
    pending=false; uint8_t status=0;
    // One GATT operation per poll, outside callbacks (BTstack may callback inline).
    switch(stage) {
        case Stage::Service:
            status=gatt_client_discover_primary_services_by_uuid128(event,connection,tgp_legacy::ServiceUuid); break;
        case Stage::Characteristic:
            status=gatt_client_discover_characteristics_for_service_by_uuid128(event,connection,&service,tgp_legacy::StateUuid); break;
        case Stage::Subscribe:
            listening=true;
            gatt_client_listen_for_characteristic_value_updates(&listener,event,connection,&characteristic);
            status=gatt_client_write_client_characteristic_configuration(event,connection,&characteristic,GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            break;
        default:break;
    }
    if(status) fail(status);
}
uint8_t version() {return wire_version;}
}
