#pragma once
#include <cstdint>
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define SCSI_SENSE_NOT_READY 2
#define SCSI_SENSE_MEDIUM_ERROR 3
#define SCSI_SENSE_ILLEGAL_REQUEST 5
#define SCSI_CMD_START_STOP_UNIT 0x1b
inline bool tusb_init() {return true;}
void tud_task();
inline bool tud_mounted() {return true;}
inline void tud_disconnect() {}
inline void tud_deinit(uint8_t) {}
inline bool tud_cdc_n_connected(uint8_t) {return false;}
inline uint32_t tud_cdc_n_write_available(uint8_t) {return CFG_TUD_CDC_TX_BUFSIZE;}
inline uint32_t tud_cdc_n_write(uint8_t,const void *,uint32_t n) {return n;}
inline void tud_cdc_n_write_flush(uint8_t) {}
inline void tud_cdc_n_read_flush(uint8_t) {}
inline uint32_t tud_cdc_n_available(uint8_t) {return 0;}
inline int tud_cdc_n_read_char(uint8_t) {return -1;}
inline void tud_msc_set_sense(uint8_t,uint8_t,uint8_t,uint8_t) {}
