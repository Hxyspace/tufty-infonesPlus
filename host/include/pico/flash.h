#pragma once
int flash_safe_execute(void (*fn)(void *),void *arg,unsigned timeout_ms);
