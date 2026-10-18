#pragma once
// Only placement annotations depend on the board SDK.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico/platform.h"
#else
#define __not_in_flash_func(name) name
#define __not_in_flash(name)
#define __time_critical_func(name) name
#endif
#ifndef INFONES_EXTENDED_MAPPERS
#define INFONES_EXTENDED_MAPPERS 1
#endif
