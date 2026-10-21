execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/deps.py" --profile host --radio --check
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Prepare business dependencies first: ${output}${error}")
endif()
set(root ${CMAKE_SOURCE_DIR})
set(runtime ${root}/app)
set(fatfs ${root}/lib/fatfs/source)
add_compile_definitions(TUFTY_HOST_PREVIEW
  LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR cJSON_NESTING_LIMIT=8
  TUFTY_FATFS_CONFIG="tufty_ffconf.h")
add_compile_options(-Wall -Wextra -Wno-unused-parameter -UNDEBUG)
include_directories(${runtime} ${root}/config ${fatfs} ${root} ${root}/presentation ${root}/platform/tufty2350
  ${root}/lib/littlefs ${root}/lib/cjson ${CMAKE_CURRENT_LIST_DIR}/stubs)
add_executable(protocol_tests protocol_tests.cpp ${runtime}/protocol.cpp ${runtime}/gamepad.cpp)
add_test(NAME protocol COMMAND protocol_tests)
add_executable(input_tests input_tests.cpp ${runtime}/gamepad.cpp ${runtime}/input.cpp)
add_test(NAME input COMMAND input_tests)
add_executable(bluetooth_manager_tests bluetooth_manager_tests.cpp ${runtime}/bluetooth_manager.cpp)
add_test(NAME bluetooth_manager COMMAND bluetooth_manager_tests)
set(btstack ${root}/lib/pico-sdk/lib/btstack)
add_executable(ble_tests ble_tests.cpp ${runtime}/ble.cpp ${runtime}/ble_gamepad.cpp ${runtime}/ble_tgp.cpp
  ${runtime}/bluetooth_manager.cpp ${runtime}/gamepad.cpp ${runtime}/input.cpp ${runtime}/controllers.cpp ${runtime}/protocol.cpp
  ${btstack}/src/btstack_hid_parser.c ${btstack}/src/btstack_util.c ${btstack}/src/ad_parser.c)
target_include_directories(ble_tests PRIVATE ${btstack}/src ${btstack}/3rd-party/micro-ecc)
target_compile_definitions(ble_tests PRIVATE ENABLE_BLE=1)
add_test(NAME ble_hid COMMAND ble_tests)
foreach(scenario sync_cancel sync_disconnect lost_cancel startup_timeout recovery_off home scan_uuid
    cancel_stale_scan cancel_late_success connect_failed request_failed
    idle_link disconnect_timeout pairing_failure physical_layout pending_link_loss
    left_stick home_combo remote_close discovery controls selection
    tgp_v1 tgp_v2 tgp_id tgp_sync tgp_handover tgp_discovery_failure tgp_subscribe_failure tgp_request_failure
    tgp_no_heartbeat tgp_sync_disconnect)
  add_test(NAME ble_${scenario} COMMAND ble_tests ${scenario})
endforeach()
add_executable(console_tests console_tests.cpp)
add_test(NAME console COMMAND console_tests)
add_executable(power_tests power_tests.cpp)
add_test(NAME power COMMAND power_tests)
add_executable(power_platform_tests power_platform_tests.cpp ${runtime}/power.cpp)
target_compile_definitions(power_platform_tests PRIVATE BTN_A=7 BTN_B=9 BTN_C=10 BTN_UP=11 BTN_DOWN=6)
add_test(NAME power_platform COMMAND power_platform_tests)
add_executable(disk_ui_tests disk_ui_tests.cpp ${runtime}/disk_ui.cpp)
add_test(NAME disk_ui COMMAND disk_ui_tests)
add_executable(usb_disk_tests usb_disk_tests.cpp ${runtime}/usb.cpp ${runtime}/protocol.cpp ${runtime}/gamepad.cpp ${runtime}/fat_disk.cpp)
target_include_directories(usb_disk_tests BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/usb_stubs)
add_test(NAME usb_disk COMMAND usb_disk_tests)
add_executable(storage_tests storage_tests.cpp ${runtime}/storage.cpp ${runtime}/gamepad.cpp ${runtime}/fat_disk.cpp
  ${runtime}/rom_store.cpp ${runtime}/menu.cpp ${runtime}/input.cpp
  ${CMAKE_CURRENT_LIST_DIR}/battery_stub.cpp
  ${fatfs}/ff.c ${fatfs}/ffunicode.c
  ${root}/lib/littlefs/lfs.c ${root}/lib/littlefs/lfs_util.c
  ${root}/lib/cjson/cJSON.c)
add_test(NAME storage_4096 COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 4096)
add_test(NAME storage_512 COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 512)
foreach(scenario valid empty max long crc offset boundary flags desc_flag no_nul header strings short truncated oversize duplicate path control)
  add_test(NAME games_index_${scenario} COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/game_index_fixture.py $<TARGET_FILE:storage_tests> index_${scenario})
endforeach()
add_test(NAME state_blank COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 4096 blank)
add_test(NAME state_corrupt_record COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 4096 corrupt)
add_test(NAME state_idle_disabled COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 4096 idle_off)
add_test(NAME state_invalid_idle COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 4096 bad_idle)
add_test(NAME state_ble COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 4096 ble)
add_test(NAME state_invalid_ble_peer COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 4096 bad_peer)
add_test(NAME state_invalid_ble_type COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 4096 bad_ble)
foreach(scenario policy bad_policy duplicate_peer gamepad_override gamepad_no_state)
  add_test(NAME state_${scenario} COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/storage_fixture.py $<TARGET_FILE:storage_tests> 4096 ${scenario})
endforeach()
add_test(NAME host_sender COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/test_sender.py)
add_test(NAME rom_preparation COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/test_roms.py)
target_link_libraries(storage_tests PRIVATE nes_cartridge)

add_test(NAME resource_images COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/../test_resource_images.py $<TARGET_FILE:storage_tests>)
