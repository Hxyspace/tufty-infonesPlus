#pragma once
#include <cstddef>
#include <cstdint>
void flash_range_erase(uint32_t at,size_t bytes);
void flash_range_program(uint32_t at,const uint8_t *data,size_t bytes);
