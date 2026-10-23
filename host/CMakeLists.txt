find_package(SDL2 2.0.18 REQUIRED CONFIG)
if(NOT Python3_EXECUTABLE)
  find_program(Python3_EXECUTABLE NAMES python3 REQUIRED)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/deps.py" --profile host --check
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Prepare host dependencies first: ${output}${error}")
endif()
set(root ${CMAKE_SOURCE_DIR})
add_executable(tufty_preview main.cpp devices.cpp flash.cpp
  ${root}/app/menu.cpp ${root}/app/ui.cpp ${root}/app/settings.cpp ${root}/app/disk_ui.cpp
  ${root}/app/storage.cpp ${root}/app/fat_disk.cpp ${root}/app/rom_store.cpp
  ${root}/app/input.cpp ${root}/app/gamepad.cpp ${root}/app/controllers.cpp
  ${root}/app/protocol.cpp ${root}/app/bluetooth_manager.cpp
  ${root}/lib/fatfs/source/ff.c ${root}/lib/fatfs/source/ffunicode.c
  ${root}/lib/littlefs/lfs.c ${root}/lib/littlefs/lfs_util.c ${root}/lib/cjson/cJSON.c)
target_compile_definitions(tufty_preview PRIVATE SDL_MAIN_HANDLED TUFTY_HOST_PREVIEW
  TUFTY_FATFS_CONFIG="tufty_ffconf.h"
  LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR cJSON_NESTING_LIMIT=8)
target_include_directories(tufty_preview PRIVATE include ${root} ${root}/app
  ${root}/config ${root}/presentation ${root}/platform/tufty2350
  ${root}/lib/fatfs/source ${root}/lib/littlefs ${root}/lib/cjson)
target_compile_options(tufty_preview PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(tufty_preview PRIVATE nes_host tufty_version SDL2::SDL2)
