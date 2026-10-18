add_library(nes_core STATIC
  InfoNES.cpp K6502.cpp InfoNES_Mapper.cpp InfoNES_Region.cpp
  InfoNES_pAPU.cpp InfoNES_pAPU_Vrc7.cpp)
target_include_directories(nes_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(nes_core PUBLIC cxx_std_17)
# Mapper implementations are included by InfoNES_Mapper.cpp, never globbed.
