// ST7789/PIO/DMA derived from tufty-nes-ble b8f1ff9 and native 16f94c2.
// Original Pimoroni MIT notices are in licenses/PIMORONI-LICENSE.
#include "panel.hpp"
#include "display_timing.hpp"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "parallel.pio.h"
#include <cmath>
#include <initializer_list>
namespace tufty::panel {
static PIO pio=pio1;
static uint sm,dma;
static Timing timing;
static void idle() {
    dma_channel_wait_for_finish_blocking(dma);
    uint32_t mask=1u<<(sm+PIO_FDEBUG_TXSTALL_LSB);
    pio->fdebug=mask;
    while(!(pio->fdebug&mask)) tight_loop_contents();
}
static void write(const void *p,size_t n) {
    dma_channel_set_trans_count(dma,n,false);
    dma_channel_set_read_addr(dma,p,true);
    idle();
}
static void command(uint8_t cmd,const char *data=nullptr,size_t n=0) {
    idle(); gpio_put(TFT_DC_PIN,0); gpio_put(TFT_CS_PIN,0);
    write(&cmd,1);
    if(n) {gpio_put(TFT_DC_PIN,1); write(data,n);}
    gpio_put(TFT_CS_PIN,1);
}
void early_init(){gpio_init(TFT_LED_PIN);gpio_put(TFT_LED_PIN,0);gpio_set_dir(TFT_LED_PIN,GPIO_OUT);}
void brightness(uint8_t percent){pwm_set_gpio_level(TFT_LED_PIN,backlight_pwm(percent/100.0f));}
void begin_frame(bool sync){
    idle();
    if(sync)timing.wait([]{return time_us_32();},[]{return gpio_get(TFT_TE_PIN);},[]{tight_loop_contents();});
    // Track clock changes without exceeding the panel's PIO instruction rate.
    pio_sm_set_clkdiv(pio,sm,ceilf(2.0f*clock_get_hz(clk_sys)/(44.0f*MHZ))*0.5f);
    uint8_t cmd=0x2c;gpio_put(TFT_DC_PIN,0);gpio_put(TFT_CS_PIN,0);
    write(&cmd,1);gpio_put(TFT_DC_PIN,1);
}
void send_column(const uint16_t *p,unsigned count){
    idle();dma_channel_set_trans_count(dma,count*2,false);dma_channel_set_read_addr(dma,p,true);
}
void end_frame(){idle();gpio_put(TFT_CS_PIN,1);}
void shutdown(){brightness(0);command(0x28);sleep_ms(20);pio_sm_set_enabled(pio,sm,false);}
SyncStatus sync_status(){return timing.status();}
void init() {
    pio_set_gpio_base(pio,16);
    sm=pio_claim_unused_sm(pio,true);
    uint offset=pio_add_program(pio,&parallel_program);
    pio_gpio_init(pio,TFT_WR_PIN);
    for(uint i=32;i<40;++i) pio_gpio_init(pio,i);
    pio_sm_set_consecutive_pindirs(pio,sm,32,8,true);
    pio_sm_set_consecutive_pindirs(pio,sm,TFT_WR_PIN,1,true);
    pio_sm_config c=parallel_program_get_default_config(offset);
    sm_config_set_out_pins(&c,32,8); sm_config_set_sideset_pins(&c,TFT_WR_PIN);
    sm_config_set_fifo_join(&c,PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&c,false,true,8);
    sm_config_set_clkdiv(&c,ceilf(2.0f*clock_get_hz(clk_sys)/(44.0f*MHZ))*0.5f);
    pio_sm_init(pio,sm,offset,&c); pio_sm_set_enabled(pio,sm,true);
    dma=dma_claim_unused_channel(true);
    dma_channel_config dc=dma_channel_get_default_config(dma);
    channel_config_set_read_increment(&dc,true);
    channel_config_set_transfer_data_size(&dc,DMA_SIZE_8);
    channel_config_set_dreq(&dc,pio_get_dreq(pio,sm,true));
    dma_channel_configure(dma,&dc,&pio->txf[sm],nullptr,0,false);
    for(uint pin : {uint(TFT_CS_PIN),uint(TFT_DC_PIN),uint(TFT_RD_PIN)}) {
        gpio_init(pin); gpio_set_dir(pin,GPIO_OUT); gpio_put(pin,1);
    }
    pwm_config pc=pwm_get_default_config(); pwm_config_set_wrap(&pc,65535);
    pwm_init(pwm_gpio_to_slice_num(TFT_LED_PIN),&pc,true);
    brightness(0); gpio_set_function(TFT_LED_PIN,GPIO_FUNC_PWM);
    command(0x01); sleep_ms(150);
    command(0x3a,"\x05",1); command(0xb2,"\x0c\x0c\x00\x33\x33",5);
    command(0xc0,"\x2c",1); command(0xc2,"\x01",1); command(0xc3,"\x0f",1);
    command(0xc4,"\x20",1); command(0xd0,"\xa4\xa1",2); command(0xc6,"\x0f",1);
    command(0xb0,"\x00\xc0",2); command(0xb7,"\x35",1); command(0xbb,"\x1b",1);
    command(0xe0,"\xf0\x00\x06\x04\x05\x05\x31\x44\x48\x36\x12\x12\x2b\x34",14);
    command(0xe1,"\xf0\x0b\x0f\x0f\x0d\x26\x31\x43\x47\x38\x14\x14\x2c\x32",14);
    command(0x21); command(0x11); sleep_ms(100);
    command(0x2a,"\x00\x00\x00\xef",4); command(0x2b,"\x00\x00\x01\x3f",4);
    command(0x36,"\x90",1);
    // A warm MCU reset leaves the LCD's GRAM intact. Clear all 320 columns
    // before DISPON, as the native driver does, with the backlight still off.
    begin_frame(false);
    alignas(4) static const uint16_t black[240]{};
    for(unsigned x=0;x<320;++x)send_column(black,240);
    end_frame();
    gpio_init(TFT_TE_PIN);gpio_set_dir(TFT_TE_PIN,GPIO_IN);
    command(0x35,"\x00",1);command(0x44,"\x00\x00",2);
    timing.configure(true);command(0x29);
}
}
