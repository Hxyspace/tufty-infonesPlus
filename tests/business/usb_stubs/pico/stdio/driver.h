#pragma once
struct stdio_driver_t {void (*out_chars)(const char *,int)=nullptr;};
inline void stdio_set_translate_crlf(stdio_driver_t *,bool) {}
inline void stdio_set_driver_enabled(stdio_driver_t *,bool) {}
