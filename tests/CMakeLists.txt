add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE nes_cartridge)
target_compile_options(core_tests PRIVATE -UNDEBUG)
add_test(NAME core_cartridge_regression COMMAND core_tests)
