#include "browser/image-carry.h"
#include <cstring>
#include <string>

namespace browser::image_carry {
namespace {
constexpr uint32_t kMagic = 0x43444353;  // "SCDC"
constexpr uint32_t kVersion = 1;

template<class T> void put(std::vector<uint8_t>& bytes, const T& value) {
  const size_t at = bytes.size();
  bytes.resize(at + sizeof(value));
  std::memcpy(bytes.data() + at, &value, sizeof(value));
}
void putBytes(std::vector<uint8_t>& bytes, std::span<const uint8_t> value) { bytes.insert(bytes.end(), value.begin(), value.end()); }

struct Reader {
  std::span<const uint8_t> bytes;
  bool ok = true;
  template<class T> T get() {
    T value{};
    if (bytes.size() < sizeof(value)) { ok = false; return value; }
    std::memcpy(&value, bytes.data(), sizeof(value));
    bytes = bytes.subspan(sizeof(value));
    return value;
  }
  std::span<const uint8_t> take(uint64_t count) {
    if (bytes.size() < count) { ok = false; return {}; }
    const auto part = bytes.first(size_t(count));
    bytes = bytes.subspan(size_t(count));
    return part;
  }
};

// parents first, as List gives them
void packTree(std::vector<uint8_t>& bytes, const TemporaryFilesystem& profile, const std::string& directory, uint32_t& count) {
  const auto entries = profile.List(directory);
  if (!entries) { return; }
  for (const auto& entry : *entries) {
    const std::string path = directory.empty() ? entry.name : directory + "\\" + entry.name;
    const auto& meta = entry.metadata;
    put(bytes, uint16_t(path.size()));
    putBytes(bytes, {reinterpret_cast<const uint8_t*>(path.data()), path.size()});
    put(bytes, uint8_t(meta.directory));
    put(bytes, meta.attributes);
    for (const auto time : {meta.creationTime, meta.lastAccessTime, meta.lastWriteTime, meta.changeTime}) { put(bytes, time); }
    std::vector<uint8_t> content(meta.directory ? 0 : size_t(meta.size));
    if (!content.empty()) { content.resize(profile.Read(path, 0, content)); }
    put(bytes, uint64_t(content.size()));
    putBytes(bytes, content);
    ++count;
    if (meta.directory) { packTree(bytes, profile, path, count); }
  }
}
}

std::vector<uint8_t> Pack(const Carry& carry, const TemporaryFilesystem& profile) {
  std::vector<uint8_t> bytes;
  put(bytes, kMagic); put(bytes, kVersion);
  put(bytes, uint32_t(carry.page.has_value()));
  if (carry.page) { putBytes(bytes, *carry.page); }
  put(bytes, uint32_t(carry.display.has_value()));
  if (carry.display) {
    put(bytes, carry.display->common); put(bytes, carry.display->format); put(bytes, carry.display->size);
    put(bytes, uint32_t(carry.display->pixels.size()));
    putBytes(bytes, carry.display->pixels);
  }
  const size_t countAt = bytes.size();
  uint32_t count = 0;
  put(bytes, count);
  packTree(bytes, profile, "", count);
  std::memcpy(bytes.data() + countAt, &count, sizeof(count));
  return bytes;
}

bool Unpack(std::span<const uint8_t> bytes, Carry& carry, TemporaryFilesystem& profile) {
  Reader in{bytes};
  if (in.get<uint32_t>() != kMagic || in.get<uint32_t>() != kVersion || !in.ok) { return false; }
  if (in.get<uint32_t>()) {
    const auto page = in.take(4096);
    if (!in.ok) { return false; }
    carry.page.emplace();
    std::memcpy(carry.page->data(), page.data(), page.size());
  }
  if (in.get<uint32_t>()) {
    Display display;
    display.common = in.get<uint32_t>(); display.format = in.get<uint32_t>(); display.size = in.get<uint32_t>();
    const auto pixels = in.take(in.get<uint32_t>());
    if (!in.ok) { return false; }
    display.pixels.assign(pixels.begin(), pixels.end());
    carry.display = std::move(display);
  }
  const uint32_t count = in.get<uint32_t>();
  for (uint32_t n = 0; n < count && in.ok; ++n) {
    const auto name = in.take(in.get<uint16_t>());
    const bool directory = in.get<uint8_t>() != 0;
    const uint32_t attributes = in.get<uint32_t>();
    std::array<uint64_t, 4> times{};
    for (auto& time : times) { time = in.get<uint64_t>(); }
    const auto content = in.take(in.get<uint64_t>());
    if (!in.ok) { return false; }
    const std::string path(reinterpret_cast<const char*>(name.data()), name.size());
    const uint32_t created = directory ? profile.CreateDirectory(path) : profile.WriteFile(path, content);
    if ((created && created != 0xc0000035) || profile.SetBasic(path, times, attributes)) { return false; }
  }
  return in.ok && in.bytes.empty();
}

}
