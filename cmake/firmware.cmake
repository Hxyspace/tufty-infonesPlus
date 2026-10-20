# Use the RAM pairing database. The SDK also lists the mutually exclusive TLV
# implementation unconditionally; do not build its Flash-backed database.
get_target_property(btstack_ble_sources pico_btstack_ble INTERFACE_SOURCES)
list(FILTER btstack_ble_sources EXCLUDE REGEX "/le_device_db_tlv\\.c$")
set_property(TARGET pico_btstack_ble PROPERTY INTERFACE_SOURCES "${btstack_ble_sources}")
# BLE-only polling architecture; the SDK's named poll target requires lwIP.
add_library(tufty_radio_poll INTERFACE)
target_link_libraries(tufty_radio_poll INTERFACE pico_cyw43_arch pico_async_context_poll)
target_compile_definitions(tufty_radio_poll INTERFACE PICO_CYW43_ARCH_POLL=1 CYW43_LWIP=0)
if(NOT PICO_SDK_VERSION_STRING VERSION_EQUAL "2.3.0")
  message(FATAL_ERROR "Restore the locked Pico SDK 2.3.0 checkout")
endif()

set(fatfs "${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/source")
add_executable(nes
  app/main.cpp presentation/display.cpp platform/tufty2350/panel.cpp platform/tufty2350/core_memory.cpp app/menu.cpp app/storage.cpp app/fat_disk.cpp
  app/rom_store.cpp app/usb.cpp app/descriptors.cpp app/protocol.cpp
  app/power.cpp platform/tufty2350/power_board.cpp platform/tufty2350/power_reset.cpp platform/tufty2350/battery.cpp
  app/disk_ui.cpp app/ble.cpp app/ble_gamepad.cpp app/ble_tgp.cpp app/bluetooth_manager.cpp
  app/gamepad.cpp app/input.cpp app/controllers.cpp platform/tufty2350/ble_stack.c platform/tufty2350/radio_wait.c
  "${fatfs}/ff.c" "${fatfs}/ffunicode.c"
  lib/littlefs/lfs.c lib/littlefs/lfs_util.c lib/cjson/cJSON.c)
target_include_directories(nes PRIVATE app presentation platform/tufty2350 config "${fatfs}"
  lib/littlefs lib/cjson)
target_compile_definitions(nes PRIVATE TUFTY2350
  TUFTY_FATFS_CONFIG="tufty_ffconf.h"
  PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x1000
  LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR LFS_NO_MALLOC cJSON_NESTING_LIMIT=8
  PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
  PICO_BTSTACK_CYW43_MAX_HCI_PROCESS_LOOP_COUNT=4)
target_link_libraries(nes_core PUBLIC pico_base_headers)
target_link_libraries(nes PRIVATE nes_host pico_stdlib pico_multicore pico_flash pico_unique_id
  hardware_psram hardware_dma hardware_pio hardware_pwm hardware_clocks
  hardware_flash hardware_powman hardware_adc tinyusb_device
  tufty_radio_poll pico_btstack_ble pico_btstack_cyw43)
target_compile_options(nes PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(nes PRIVATE "LINKER:-T,${CMAKE_CURRENT_SOURCE_DIR}/cmake/firmware_limit.ld"
  "LINKER:--print-memory-usage" "LINKER:--wrap=btstack_cyw43_init"
  "LINKER:--wrap=cyw43_await_background_or_timeout_us"
  "LINKER:--wrap=cyw43_delay_ms" "LINKER:--wrap=cyw43_delay_us")
set_property(TARGET nes APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/firmware_limit.ld")
pico_generate_pio_header(nes "${CMAKE_CURRENT_SOURCE_DIR}/platform/tufty2350/parallel.pio")
pico_enable_stdio_uart(nes 0)
pico_enable_stdio_usb(nes 0)
pico_set_program_name(nes "Tufty NES Flash")
pico_set_program_version(nes "${PROJECT_VERSION}")
set_target_properties(nes PROPERTIES OUTPUT_NAME tufty-nes-flash)
# Preserve the SDK/native tagged compatibility block behavior; check separately.
pico_add_extra_outputs(nes)
