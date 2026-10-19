#pragma once
inline int flash_safe_execute(void (*fn)(void *),void *arg,unsigned) {fn(arg); return 0;}
