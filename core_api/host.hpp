#pragma once
#include <cstddef>
#include <cstdint>
namespace nes {
struct Host {
  void (*service)()=nullptr;
  void (*input)(std::uint8_t &pad1,std::uint8_t &pad2,bool &quit)=nullptr;
  void (*line)(unsigned y,const std::uint16_t *pixels,std::uint8_t mask,bool pal)=nullptr;
  void (*frame)()=nullptr;
  void (*message)(const char *)=nullptr;
};
namespace infones_adapter {
// Synchronous single-instance run. Every callback is borrowed until return.
// No callback may reenter run/load/unload. Null audio output still clocks APU.
bool run(const Host &host,const std::uint8_t *rom,std::size_t bytes,const char **error=nullptr);
}
}
