/*
 * Board definition for Pimoroni Tufty 2350
 * RP2350B + 16MB flash + 8MB PSRAM
 * 2.8" 320x240 IPS LCD (ST7789, 8-bit parallel)
 */

#ifndef _BOARDS_TUFTY2350_H
#define _BOARDS_TUFTY2350_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 1)

#define PICO_RP2350A 0  // RP2350B (not A)

// --- UART ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- I2C (QWIIC/STEMMA QT) ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 4
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 5
#endif

// --- FLASH ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#include "boards/pico2.h"
// pico2.h describes the 30-GPIO A package. Tufty uses the 48-GPIO B.
#undef PICO_RP2350A
#define PICO_RP2350A 0

// SDK startup sets clock/voltage and initializes QMI before either core runs.
#define PLL_SYS_REFDIV 1
#define PLL_SYS_VCO_FREQ_HZ 1500000000
#define PLL_SYS_POSTDIV1 6
#define PLL_SYS_POSTDIV2 1
#define SYS_CLK_HZ 250000000
#define SYS_CLK_VREG_VOLTAGE_MIN VREG_VOLTAGE_1_20
#define SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST 1
#define PICO_PSRAM_CS_PIN 8
#define PICO_PSRAM_SIZE_BYTES (8 * 1024 * 1024)

// --- LCD (8-bit parallel ST7789) ---
#define TFT_CS_PIN     27
#define TFT_DC_PIN     28   // RS/Data-Command
#define TFT_WR_PIN     30   // Write strobe
#define TFT_RD_PIN     31   // Read strobe (active low, idle high)
#define TFT_LED_PIN    26   // Backlight
#define TFT_TE_PIN     21
#define TFT_DB_BASE    32   // DB0=GPIO32 .. DB7=GPIO39

// --- Buttons ---
#define BTN_A          7
#define BTN_B          9
#define BTN_C          10
#define BTN_UP         11
#define BTN_DOWN       6
#define BTN_HOME       22

// Shared action-key interrupt and always-on wake circuitry.
#define BW_SWITCH_A    BTN_A
#define BW_SWITCH_B    BTN_B
#define BW_SWITCH_C    BTN_C
#define BW_SWITCH_UP   BTN_UP
#define BW_SWITCH_DOWN BTN_DOWN
#define BW_SWITCH_HOME BTN_HOME
#define BW_RESET_SW    14
#define BW_VBUS_DETECT 12
#define BW_RTC_ALARM   13
#define BW_SWITCH_INT  15
#define BW_SW_POWER_EN 41
#define BW_VBAT_SENSE  40
#define BW_SENSE_1V1   42
#define BW_SWITCH_MASK ((1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C) | (1u << BTN_UP) | (1u << BTN_DOWN))

// --- PSRAM ---
#define BW_PSRAM_CS    8

// --- Rear LEDs ---
#define BW_LED_0       0
#define BW_LED_1       1
#define BW_LED_2       2
#define BW_LED_3       3

// Override pico2.h's PICO_DEFAULT_LED_PIN (25 = WL_CS on Tufty!)
#undef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN BW_LED_0

// CYW43439 radio; USB disk mode leaves WL_ON low.
#define CYW43_WL_GPIO_COUNT 3
#define CYW43_DEFAULT_PIN_WL_REG_ON 23u
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 24u
#define CYW43_DEFAULT_PIN_WL_DATA_IN 24u
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 24u
#define CYW43_DEFAULT_PIN_WL_CLOCK 29u
#define CYW43_DEFAULT_PIN_WL_CS 25u
#define CYW43_PIO_CLOCK_DIV_INT 3

#endif // _BOARDS_TUFTY2350_H
