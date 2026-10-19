#pragma once
#include <cstdint>
#include "layout.hpp"

namespace tufty::storage {
enum class RomResult : uint8_t { Ok, NotReady, Corrupt, InvalidPath, NotFound, NotDirectory, End, Busy };
struct RomView {
  const uint8_t *data = nullptr;
  uint32_t bytes = 0;
  bool directory = false;
};
struct RomEntry {
  const uint8_t *name = nullptr;
  uint32_t name_bytes = 0;
  RomView view{};
};
class RomStore {
 public:
  RomStore() = default;
  RomStore(const RomStore &) = delete;
  RomStore &operator=(const RomStore &) = delete;
  // Compatible with MicroPython RM1 records, without the Python runtime.
  RomResult mount(const void *image, uint32_t bytes);
  RomResult unmount(); // VFS leases also prevent replacing the image.
  RomResult find(const char *path, RomView &view) const;
  RomResult next(const RomView &directory, uint32_t &cursor, RomEntry &entry) const;
  RomResult read(const RomView &file, uint32_t offset, void *buffer,
                 uint32_t requested, uint32_t &actual) const;
  bool mounted() const { return base_ != nullptr; }
  uint32_t image_bytes() const { return end_; }
 private:
  friend class Vfs;
  uint32_t read_leases_ = 0;
  struct Record { uint32_t kind, payload, end; };
  bool decode(uint32_t &cursor, uint32_t end, uint32_t &value) const;
  bool record(uint32_t &cursor, uint32_t end, Record &record) const;
  bool named(const Record &record, RomEntry &entry) const;
  bool data(uint32_t cursor, uint32_t end, RomView &view) const;
  bool validate(uint32_t cursor, uint32_t end, uint32_t depth) const;
  bool bounds(const RomView &view, uint32_t &offset) const;
  const uint8_t *base_ = nullptr;
  uint32_t capacity_ = 0, begin_ = 0, end_ = 0;
};
}
