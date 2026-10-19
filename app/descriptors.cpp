#include "usb.hpp"
#include "tusb.h"
#include "pico/unique_id.h"
#include <cstring>
namespace {
tusb_desc_device_t desc={sizeof(tusb_desc_device_t),TUSB_DESC_DEVICE,0x0200,
    TUSB_CLASS_MISC,MISC_SUBCLASS_COMMON,MISC_PROTOCOL_IAD,64,usb::vid,usb::runtime_pid,0x0100,1,2,3,1};
const uint8_t normal[]={
    TUD_CONFIG_DESCRIPTOR(1,4,0,TUD_CONFIG_DESC_LEN+2*TUD_CDC_DESC_LEN,0,250),
    TUD_CDC_DESCRIPTOR(0,4,0x81,8,0x02,0x82,64),
    TUD_CDC_DESCRIPTOR(2,5,0x84,8,0x05,0x85,64)
};
const uint8_t disk[]={
    TUD_CONFIG_DESCRIPTOR(1,5,0,TUD_CONFIG_DESC_LEN+2*TUD_CDC_DESC_LEN+TUD_MSC_DESC_LEN,0,250),
    TUD_CDC_DESCRIPTOR(0,4,0x81,8,0x02,0x82,64),
    TUD_CDC_DESCRIPTOR(2,5,0x84,8,0x05,0x85,64),
    TUD_MSC_DESCRIPTOR(4,6,0x03,0x83,64)
};
}
extern "C" {
const uint8_t *tud_descriptor_device_cb() {
    desc.idProduct=usb::disk_mode()?usb::disk_pid:usb::runtime_pid;
    return reinterpret_cast<const uint8_t *>(&desc);
}
const uint8_t *tud_descriptor_configuration_cb(uint8_t) {return usb::disk_mode()?disk:normal;}
const uint16_t *tud_descriptor_string_cb(uint8_t index,uint16_t) {
    static uint16_t out[33]; static char serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES*2+1];
    if(!index) {out[0]=(TUSB_DESC_STRING<<8)|4; out[1]=0x0409; return out;}
    pico_get_unique_board_id_string(serial,sizeof(serial));
    const char *strings[]={nullptr,"Tufty NES","Tufty NES Flash",serial,"Console","Gamepad","System disk"};
    if(index>=sizeof(strings)/sizeof(strings[0])) return nullptr;
    size_t n=std::strlen(strings[index]); if(n>32) n=32;
    for(size_t i=0;i<n;++i) out[i+1]=strings[index][i];
    out[0]=(TUSB_DESC_STRING<<8)|(2*n+2); return out;
}
}
