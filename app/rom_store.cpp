#include "rom_store.hpp"
#include <cstring>

namespace tufty::storage {
namespace { constexpr uint32_t kFilesystem = 0x14a6b1; }
bool RomStore::decode(uint32_t &cursor, uint32_t end, uint32_t &value) const {
  value = 0;
  for (uint32_t i = 0; i < 10 && cursor < end; ++i) {
    const uint8_t byte = base_[cursor++];
    if (value > (UINT32_MAX >> 7)) return false;
    value = (value << 7) | (byte & 127);
    if (!(byte & 128)) return true;
  }
  return false;
}
bool RomStore::record(uint32_t &cursor, uint32_t end, Record &r) const {
  uint32_t size;
  if (!decode(cursor, end, r.kind) || !r.kind || !decode(cursor, end, size) ||
      size > end - cursor) return false;
  r.payload = cursor; r.end = cursor + size; cursor = r.end;
  return true;
}
bool RomStore::data(uint32_t cursor, uint32_t end, RomView &view) const {
  while (cursor < end) {
    Record r;
    if (!record(cursor, end, r)) return false;
    if (r.kind == 2) { view = {base_ + r.payload, r.end - r.payload, false}; return true; }
    if (r.kind == 3) {
      uint32_t p = r.payload, size, offset;
      if (!decode(p, r.end, size) || !decode(p, r.end, offset) ||
          offset > end_ - begin_ || size > end_ - begin_ - offset) return false;
      view = {base_ + begin_ + offset, size, false}; return true;
    }
  }
  return false;
}
bool RomStore::named(const Record &r, RomEntry &entry) const {
  uint32_t p = r.payload, length;
  if (!decode(p, r.end, length) || !length || length > r.end - p) return false;
  for (uint32_t i = 0; i < length; ++i)
    if (!base_[p + i] || base_[p + i] == '/' || base_[p + i] == '\\') return false;
  entry.name = base_ + p; entry.name_bytes = length; p += length;
  if (r.kind == 4) { entry.view = {base_ + p, r.end - p, true}; return true; }
  return data(p, r.end, entry.view);
}
bool RomStore::validate(uint32_t cursor, uint32_t end, uint32_t depth) const {
  if (depth > 16) return false;
  while (cursor < end) {
    Record r;
    if (!record(cursor, end, r)) return false;
    if (r.kind == 4 || r.kind == 5) {
      RomEntry entry;
      if (!named(r, entry)) return false;
      const uint32_t p = r.payload;
      uint32_t child = p, length;
      if (!decode(child, r.end, length)) return false;
      child += length;
      if (!validate(child, r.end, depth + 1)) return false;
    } else if (r.kind == 3) {
      uint32_t p = r.payload, size, offset;
      if (!decode(p, r.end, size) || !decode(p, r.end, offset) ||
          offset > end_ - begin_ || size > end_ - begin_ - offset) return false;
    }
    // Padding and unknown metadata are skipped, as specified by RM1.
  }
  return cursor == end;
}
RomResult RomStore::unmount() {
  if (read_leases_) return RomResult::Busy;
  base_ = nullptr; capacity_ = begin_ = end_ = 0;
  return RomResult::Ok;
}
RomResult RomStore::mount(const void *image, uint32_t bytes) {
  if (unmount() != RomResult::Ok) return RomResult::Busy;
  if (!image || bytes < 4 || bytes > layout::kRom.bytes) return RomResult::Corrupt;
  base_ = static_cast<const uint8_t *>(image); capacity_ = bytes;
  Record r; uint32_t p = 0;
  if (base_[0] != 0xd2 || base_[1] != 0xcd || base_[2] != 0x31 ||
      !record(p, bytes, r) || r.kind != kFilesystem || (r.end & 1)) {
    unmount(); return RomResult::Corrupt;
  }
  begin_ = r.payload; end_ = r.end;
  if (!validate(begin_, end_, 0)) { unmount(); return RomResult::Corrupt; }
  return RomResult::Ok;
}
bool RomStore::bounds(const RomView &view, uint32_t &offset) const {
  if (!base_ || !view.data) return false;
  const uintptr_t start = reinterpret_cast<uintptr_t>(base_);
  const uintptr_t address = reinterpret_cast<uintptr_t>(view.data);
  if (address < start || address - start > end_) return false;
  offset = static_cast<uint32_t>(address - start);
  return offset >= begin_ && view.bytes <= end_ - offset;
}
RomResult RomStore::next(const RomView &directory, uint32_t &cursor, RomEntry &entry) const {
  entry = {};
  if (!base_) return RomResult::NotReady;
  uint32_t start;
  if (!directory.directory) return RomResult::NotDirectory;
  if (!bounds(directory, start) || cursor > directory.bytes) return RomResult::Corrupt;
  uint32_t p = start + cursor;
  while (p < start + directory.bytes) {
    Record r;
    if (!record(p, start + directory.bytes, r)) return RomResult::Corrupt;
    cursor = p - start;
    if (r.kind == 4 || r.kind == 5)
      return named(r, entry) ? RomResult::Ok : RomResult::Corrupt;
  }
  return RomResult::End;
}
RomResult RomStore::find(const char *path, RomView &view) const {
  view = {};
  if (!base_) return RomResult::NotReady;
  if (!path || std::strncmp(path, "/rom", 4) || (path[4] && path[4] != '/'))
    return RomResult::InvalidPath;
  uint32_t length = 0;
  while (length <= 1024 && path[length]) ++length;
  if (length > 1024) return RomResult::InvalidPath;
  RomView current{base_ + begin_, end_ - begin_, true};
  const char *part = path + 4;
  if (*part) ++part;
  uint32_t depth = 0;
  while (*part) {
    if (++depth > 16) return RomResult::InvalidPath;
    const char *end = part;
    while (*end && *end != '/') {
      if (static_cast<uint8_t>(*end) < 32 || *end == '\\' || *end == ':' || *end == 127)
        return RomResult::InvalidPath;
      ++end;
    }
    const uint32_t n = end - part;
    if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.'))
      return RomResult::InvalidPath;
    uint32_t cursor = 0; RomEntry entry; bool found = false;
    while (true) {
      const auto result = next(current, cursor, entry);
      if (result == RomResult::End) break;
      if (result != RomResult::Ok) return result;
      if (entry.name_bytes == n && !std::memcmp(entry.name, part, n)) {
        current = entry.view; found = true; break;
      }
    }
    if (!found) return RomResult::NotFound;
    if (*end && !current.directory) return RomResult::NotDirectory;
    part = *end ? end + 1 : end;
  }
  view = current; return RomResult::Ok;
}
RomResult RomStore::read(const RomView &file, uint32_t offset, void *buffer,
                         uint32_t requested, uint32_t &actual) const {
  actual = 0;
  if (!base_) return RomResult::NotReady;
  uint32_t start;
  if (file.directory || !bounds(file, start) || offset > file.bytes || (requested && !buffer))
    return RomResult::Corrupt;
  actual = requested < file.bytes - offset ? requested : file.bytes - offset;
  if (actual) std::memcpy(buffer, file.data + offset, actual);
  return RomResult::Ok;
}
}
