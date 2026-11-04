# Firmware and SDL source lists. Tests keep their own minimal source selections.
get_filename_component(TUFTY_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(TUFTY_FIRMWARE_SOURCES
  app/main.cpp
  presentation/display.cpp
  presentation/game_view.cpp
  presentation/disk_display.cpp
  presentation/disk_renderer.cpp
  platform/tufty2350/panel.cpp
  platform/tufty2350/core_memory.cpp
  app/menu.cpp
  app/storage.cpp
  app/fat_disk.cpp
  app/rom_store.cpp
  app/library.cpp
  app/game_lists.cpp
  app/usb.cpp
  app/shell.cpp
  app/descriptors.cpp
  app/protocol.cpp
  app/settings.cpp
  app/gamepad_test.cpp
  app/background.cpp
  app/side_background.cpp
  app/ui.cpp
  app/power.cpp
  platform/tufty2350/power_board.cpp
  platform/tufty2350/power_reset.cpp
  platform/tufty2350/battery.cpp
  app/disk_ui.cpp
  app/ble.cpp
  app/ble_gamepad.cpp
  app/ble_tgp.cpp
  app/bluetooth_manager.cpp
  app/gamepad.cpp
  app/input.cpp
  app/controllers.cpp
  platform/tufty2350/ble_stack.c
  platform/tufty2350/radio_wait.c
  lib/fatfs/source/ff.c
  lib/fatfs/source/ffunicode.c
  lib/littlefs/lfs.c
  lib/littlefs/lfs_util.c
  lib/cjson/cJSON.c)

set(TUFTY_PREVIEW_SOURCES
  host/main.cpp
  host/devices.cpp
  host/flash.cpp
  app/menu.cpp
  app/ui.cpp
  app/settings.cpp
  app/gamepad_test.cpp
  app/background.cpp
  app/side_background.cpp
  presentation/game_view.cpp
  app/disk_ui.cpp
  app/storage.cpp
  app/library.cpp
  app/game_lists.cpp
  app/fat_disk.cpp
  app/rom_store.cpp
  app/input.cpp
  app/gamepad.cpp
  app/controllers.cpp
  app/protocol.cpp
  app/bluetooth_manager.cpp
  lib/fatfs/source/ff.c
  lib/fatfs/source/ffunicode.c
  lib/littlefs/lfs.c
  lib/littlefs/lfs_util.c
  lib/cjson/cJSON.c)

# Resolve paths here so both the root target and host/ can use these lists.
list(TRANSFORM TUFTY_FIRMWARE_SOURCES PREPEND "${TUFTY_SOURCE_ROOT}/")
list(TRANSFORM TUFTY_PREVIEW_SOURCES PREPEND "${TUFTY_SOURCE_ROOT}/")
