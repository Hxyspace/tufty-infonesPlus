#include "ble_gamepad.hpp"
#include "gamepad.hpp"
#include "pico/stdlib.h"
#include "btstack.h"
#include <cstdio>

namespace ble_gamepad {
namespace {
Notify notify=nullptr;
bool tracing=false,discovering=false,ready=false,remote_close=false;
uint16_t cid=0;
uint8_t service_index=0,report_id=0;
uint8_t descriptor_storage[1024];
gamepad::Receiver *receiver=nullptr;
uint32_t now() {return to_ms_since_boot(get_absolute_time());}
void trace_bytes(const uint8_t *data,size_t bytes) {
    for(size_t i=0;i<bytes;++i) std::printf("%s%02X",i?" ":"",data[i]);
    std::printf("\n");
}
// Check the Report Map before interpreting XTARK byte offsets. In particular,
// do not reinterpret a keyboard/mouse/consumer report as the XTARK gamepad.
bool button_layout(const uint8_t *map,uint16_t bytes,uint16_t id) {
    uint16_t fields=0;
    uint8_t analog=0;
    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator,map,bytes,HID_REPORT_TYPE_INPUT);
    while(btstack_hid_usage_iterator_has_more(&iterator)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator,&item);
        if(item.report_id!=id) continue;
        // The tenth byte is constant padding in the physical XTARK descriptor.
        if(item.bit_pos+item.size>72) return false;
        int channel=-1;
        if(item.usage_page==1) {
            switch(item.usage) {case 0x30:channel=0;break; case 0x31:channel=1;break;
                case 0x32:channel=2;break; case 0x35:channel=3;break; default:break;}
        } else if(item.usage_page==2) {
            if(item.usage==0xc4) channel=4;
            if(item.usage==0xc5) channel=5;
        }
        if(channel>=0) {
            unsigned offset=channel<4?unsigned(channel)*8:56+unsigned(channel-4)*8;
            if(item.bit_pos!=offset || item.size!=8 || item.global_logical_minimum!=0 ||
               iterator.global_logical_maximum!=255 || (item.descriptor_item.item_value&7)!=2) return false;
            analog|=uint8_t(1u<<channel);
        }
        if(item.usage_page!=9 || item.size!=1 || (item.descriptor_item.item_value&7)!=2) continue;
        if(item.usage>=1 && item.usage<=16 && item.bit_pos==39+item.usage)
            fields|=uint16_t(1u<<(item.usage-1));
    }
    return fields==0xffff && analog==0x3f;
}
bool select_layout(uint8_t instances) {
    for(uint8_t index=0;index<instances;++index) {
        const uint8_t *map=hids_host_descriptor_storage_get_descriptor_data(cid,index);
        uint16_t bytes=hids_host_descriptor_storage_get_descriptor_len(cid,index);
        if(tracing) {
            std::printf("BLE: REPORT MAP service=%u bytes=%u data=",index,bytes);
            trace_bytes(map,map?bytes:0);
        }
        if(!map || !bytes) continue;
        btstack_hid_usage_iterator_t iterator;
        btstack_hid_usage_iterator_init(&iterator,map,bytes,HID_REPORT_TYPE_INPUT);
        while(btstack_hid_usage_iterator_has_more(&iterator)) {
            btstack_hid_usage_item_t item;
            btstack_hid_usage_iterator_get_item(&iterator,&item);
            if(item.usage_page!=1 || item.usage!=0x39 || item.bit_pos!=32 ||
               item.size!=4 || item.global_logical_minimum!=0 ||
               iterator.global_logical_maximum!=7 || (item.descriptor_item.item_value&0x47)!=0x42) continue;
            if(btstack_hid_get_report_size_for_id(item.report_id,HID_REPORT_TYPE_INPUT,map,bytes)!=gamepad::ReportBytes ||
               !button_layout(map,bytes,item.report_id)) continue;
            service_index=index;
            report_id=item.report_id==HID_REPORT_ID_UNDEFINED?0:uint8_t(item.report_id);
            return true;
        }
    }
    std::printf("BLE: INCOMPATIBLE REPORT MAP; use ble trace on, then ble scan\n");
    return false;
}
void hid_event(uint8_t packet_type,uint16_t,uint8_t *packet,uint16_t size) {
    if((packet_type!=HCI_EVENT_PACKET && packet_type!=HCI_EVENT_GATTSERVICE_META) ||
       size<5 || packet[0]!=HCI_EVENT_GATTSERVICE_META || little_endian_read_16(packet,3)!=cid) return;
    switch(packet[2]) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            if(size<8 || !discovering) return;
            uint8_t status=gattservice_subevent_hid_service_connected_get_status(packet);
            if(status || !select_layout(gattservice_subevent_hid_service_connected_get_num_instances(packet))) {
                notify(Event::Failed,status?status:ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE); return;
            }
            discovering=false; ready=true; receiver->release();
            notify(Event::Ready,report_id);
            // Fetch held state even from physical controllers that only notify changes.
            hids_host_send_get_report(cid,report_id,HID_REPORT_TYPE_INPUT);
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            if(!ready || size<7 || packet[5]!=service_index || packet[6]!=report_id) return;
            if(size<9) {receiver->receive(nullptr,0); return;}
            uint16_t bytes=gattservice_subevent_hid_report_get_report_len(packet);
            const uint8_t *report=gattservice_subevent_hid_report_get_report(packet);
            // BTstack prepends the Report ID, although GATT notifications do not.
            if(bytes!=gamepad::ReportBytes+1 || size<9+bytes || report[0]!=report_id) {
                receiver->receive(nullptr,0); return;
            }
            if(remote_close) return;
            if(gamepad::disconnect_report(report+1,bytes-1)) {
                receiver->release(); remote_close=true;
                notify(Event::RemoteClose,0);
                return;
            }
            if(receiver->receive(report+1,bytes-1,now())) {
                if(receiver->contributing() && (receiver->nes() || receiver->home)) notify(Event::Activity,0);
            }
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            reset();
            break;
        default:break;
    }
}
}
void init(gamepad::Receiver &input,Notify callback) {
    receiver=&input; notify=callback; hids_host_init(descriptor_storage,sizeof(descriptor_storage));
}
uint8_t connect(uint16_t handle) {
    reset(); discovering=true;
    return hids_host_connect(handle,hid_event,HID_PROTOCOL_MODE_REPORT,&cid);
}
void reset() {if(receiver) receiver->release(); cid=0; discovering=ready=remote_close=false;}
void trace(bool enabled) {tracing=enabled;}
uint8_t service() {return service_index;}
uint8_t report() {return report_id;}
}
