#include "browser/temporary-filesystem.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace browser {

TemporaryFilesystem::TemporaryFilesystem(uint64_t capacity, size_t maximumEntries, std::function<uint64_t()> clock)
  : clock_(clock ? std::move(clock) : [] {
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 100) + 116444736000000000ull;
    }), capacity_(capacity), maximumEntries_(maximumEntries) {
  if (capacity < AllocationUnit || capacity % AllocationUnit || capacity > MaximumCapacity ||
      !maximumEntries || maximumEntries > 4096) {
    throw std::invalid_argument("invalid temporary filesystem bounds");
  }
  nodes_.emplace("", NewNode(true, 0x10));
}

TemporaryFilesystem::Node TemporaryFilesystem::NewNode(bool directory, uint32_t attributes, std::vector<uint8_t> bytes) const {
  const auto now = clock_();
  return {directory, std::move(bytes), attributes, now, now, now, now};
}

std::optional<std::string> TemporaryFilesystem::Normalize(std::string path) {
  if (path.size() > 240) { return {}; }
  for (char& value : path) {
    if (uint8_t(value) < 0x20 || uint8_t(value) >= 0x7f || value == '/' || value == ':' ||
        value == '*' || value == '?' || value == '"') { return {}; }
    if (value >= 'a' && value <= 'z') { value -= 'a' - 'A'; }
  }
  size_t start = 0;
  while (start < path.size()) {
    const auto end = path.find('\\', start);
    const auto part = path.substr(start, end == std::string::npos ? end : end - start);
    if (part.empty() || part == "." || part == "..") { return {}; }
    if (end == std::string::npos) { break; }
    start = end + 1;
    if (start == path.size()) { return {}; }
  }
  return path;
}

uint64_t TemporaryFilesystem::Allocation(uint64_t size) {
  return ((size + AllocationUnit - 1) / AllocationUnit) * AllocationUnit;
}

uint32_t TemporaryFilesystem::ParentStatus(const std::string& path) const {
  const auto separator = path.rfind('\\');
  const auto parent = nodes_.find(separator == std::string::npos ? "" : path.substr(0, separator));
  return parent != nodes_.end() && parent->second.directory ? 0 : 0xc000003a;
}

std::optional<TemporaryFilesystem::Metadata> TemporaryFilesystem::Stat(const std::string& path) const {
  const auto normalized = Normalize(path);
  if (!normalized) { return {}; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*normalized);
  if (node == nodes_.end()) { return {}; }
  const auto& value = node->second;
  return Metadata{value.directory, value.bytes.size(), value.attributes, value.creationTime, value.lastAccessTime, value.lastWriteTime, value.changeTime, std::max(Allocation(value.bytes.size()), value.reservation)};
}

std::optional<std::vector<TemporaryFilesystem::DirectoryEntry>> TemporaryFilesystem::List(const std::string& path) const {
  const auto normalized = Normalize(path);
  if (!normalized) { return {}; }
  std::lock_guard lock(lock_);
  const auto parent = nodes_.find(*normalized);
  if (parent == nodes_.end() || !parent->second.directory) { return {}; }
  std::vector<DirectoryEntry> entries;
  const auto prefix = normalized->empty() ? "" : *normalized + "\\";
  for (auto node = nodes_.lower_bound(prefix); node != nodes_.end() && node->first.starts_with(prefix); ++node) {
    const auto name = node->first.substr(prefix.size());
    if (name.empty() || name.find('\\') != std::string::npos) { continue; }
    const auto& value = node->second;
    entries.push_back({name, {value.directory, value.bytes.size(), value.attributes,
      value.creationTime, value.lastAccessTime, value.lastWriteTime, value.changeTime, std::max(Allocation(value.bytes.size()), value.reservation)}});
  }
  return entries;
}

TemporaryFilesystem::Accounting TemporaryFilesystem::Usage() const {
  std::lock_guard lock(lock_);
  return {capacity_, allocated_, nodes_.size()};
}

uint32_t TemporaryFilesystem::CreateDirectory(const std::string& path) {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  if (nodes_.contains(*normalized)) { return 0xc0000035; }
  if (const auto status = ParentStatus(*normalized)) { return status; }
  if (nodes_.size() >= maximumEntries_) { return 0xc000009a; }
  if (capacity_ - allocated_ < AllocationUnit) { return 0xc000007f; }
  nodes_.emplace(*normalized, NewNode(true, 0x10));
  allocated_ += AllocationUnit;
  ++revision_;
  return 0;
}

uint32_t TemporaryFilesystem::CreateFile(const std::string& path, uint32_t attributes, uint64_t allocation) {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  if (nodes_.contains(*normalized)) { return 0xc0000035; }
  if (const auto status = ParentStatus(*normalized)) { return status; }
  if (nodes_.size() >= maximumEntries_) { return 0xc000009a; }
  if (allocation >> 63) { return 0xc000000d; }
  if (allocation > capacity_ || Allocation(allocation) > capacity_ - allocated_) { return 0xc000007f; }
  auto node = NewNode(false, attributes);
  node.reservation = Allocation(allocation);
  nodes_.emplace(*normalized, std::move(node));
  allocated_ += Allocation(allocation);
  ++revision_;
  return 0;
}

uint32_t TemporaryFilesystem::OverwriteFile(const std::string& path, uint32_t attributes) {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*normalized);
  if (node == nodes_.end()) { return ParentStatus(*normalized) ? 0xc000003a : 0xc0000034; }
  if (node->second.directory) { return 0xc00000ba; }
  if (node->second.attributes & 1) { return 0xc0000022; }
  const auto now = clock_();
  allocated_ -= std::max(Allocation(node->second.bytes.size()), node->second.reservation);
  node->second.reservation = 0;
  std::vector<uint8_t>().swap(node->second.bytes);
  node->second.attributes = attributes;
  node->second.lastWriteTime = now;
  node->second.changeTime = now;
  ++revision_;
  return 0;
}

uint32_t TemporaryFilesystem::WriteFile(const std::string& path, std::span<const uint8_t> bytes) {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*normalized);
  if (node != nodes_.end() && node->second.directory) { return 0xc00000ba; }
  if (node != nodes_.end() && (node->second.attributes & 1)) { return 0xc0000022; }
  if (const auto status = ParentStatus(*normalized)) { return status; }
  if (node == nodes_.end() && nodes_.size() >= maximumEntries_) { return 0xc000009a; }
  const auto previous = node == nodes_.end() ? 0 : std::max(Allocation(node->second.bytes.size()), node->second.reservation);
  const auto allocation = std::max(Allocation(bytes.size()), node == nodes_.end() ? uint64_t(0) : node->second.reservation);
  if (bytes.size() > capacity_ || allocation > capacity_ - allocated_ + previous) { return 0xc000007f; }
  std::vector<uint8_t> replacement(bytes.begin(), bytes.end());
  if (node == nodes_.end()) { nodes_.emplace(*normalized, NewNode(false, 0x80, std::move(replacement))); }
  else {
    const auto now = clock_();
    node->second.bytes.swap(replacement);
    node->second.lastWriteTime = now;
    node->second.changeTime = now;
  }
  allocated_ = allocated_ - previous + allocation;
  ++revision_;
  return 0;
}

uint32_t TemporaryFilesystem::Delete(const std::string& path) {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*normalized);
  if (node == nodes_.end()) { return 0xc0000034; }
  if (normalized->empty() || (node->second.attributes & 1)) { return 0xc0000022; }
  const auto next = nodes_.lower_bound(*normalized + "\\");
  if (node->second.directory && next != nodes_.end() && next->first.starts_with(*normalized + "\\")) { return 0xc0000101; }
  allocated_ -= node->second.directory ? AllocationUnit : std::max(Allocation(node->second.bytes.size()), node->second.reservation);
  nodes_.erase(node);
  ++revision_;
  return 0;
}

uint32_t TemporaryFilesystem::Rename(const std::string& source, const std::string& destination, bool replace) {
  const auto from = Normalize(source), to = Normalize(destination);
  if (!from || !to) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*from);
  if (node == nodes_.end()) { return 0xc0000034; }
  if (from->empty() || to->empty() || (node->second.attributes & 1)) { return 0xc0000022; }
  if (node->second.directory) { return 0xc00000bb; }
  if (*from == *to) { return 0; }
  if (const auto status = ParentStatus(*to)) { return status; }
  const auto target = nodes_.find(*to);
  if (target != nodes_.end()) {
    if (!replace) { return 0xc0000035; }
    if (target->second.directory || (target->second.attributes & 1)) { return 0xc0000022; }
  }
  const auto now = clock_();
  std::string replacement = *to;
  if (target != nodes_.end()) {
    allocated_ -= std::max(Allocation(target->second.bytes.size()), target->second.reservation);
    nodes_.erase(target);
  }
  auto moved = nodes_.extract(node);
  moved.key().swap(replacement);
  moved.mapped().changeTime = now;
  nodes_.insert(std::move(moved));
  ++revision_;
  return 0;
}

uint32_t TemporaryFilesystem::Resize(const std::string& path, uint64_t size) {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*normalized);
  if (node == nodes_.end()) { return 0xc0000034; }
  if (node->second.directory) { return 0xc00000ba; }
  if (node->second.attributes & 1) { return 0xc0000022; }
  if (size >> 63) { return 0xc000000d; }
  if (size > capacity_) { return 0xc000007f; }
  const auto previous = std::max(Allocation(node->second.bytes.size()), node->second.reservation);
  const auto reservation = size < node->second.bytes.size() ? std::min(node->second.reservation, Allocation(size)) : node->second.reservation;
  const auto allocation = std::max(Allocation(size), reservation);
  if (allocation > capacity_ - allocated_ + previous) { return 0xc000007f; }
  const auto now = clock_();
  node->second.bytes.resize(size);
  node->second.reservation = reservation;
  node->second.lastWriteTime = now;
  node->second.changeTime = now;
  allocated_ = allocated_ - previous + allocation;
  ++revision_;
  return 0;
}

uint32_t TemporaryFilesystem::Reserve(const std::string& path, uint64_t size) {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*normalized);
  if (node == nodes_.end()) { return 0xc0000034; }
  if (node->second.directory) { return 0xc00000ba; }
  if (node->second.attributes & 1) { return 0xc0000022; }
  if (size >> 63) { return 0xc000000d; }
  if (size > capacity_) { return 0xc000007f; }
  const auto previous = std::max(Allocation(node->second.bytes.size()), node->second.reservation);
  const auto allocation = Allocation(size);
  if (allocation > capacity_ - allocated_ + previous) { return 0xc000007f; }
  const auto now = clock_();
  if (size < node->second.bytes.size()) { node->second.bytes.resize(size); node->second.lastWriteTime = now; }
  node->second.reservation = allocation;
  node->second.changeTime = now;
  allocated_ = allocated_ - previous + allocation;
  ++revision_;
  return 0;
}

uint32_t TemporaryFilesystem::SetBasic(const std::string& path, const std::array<uint64_t, 4>& times, uint32_t attributes) {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*normalized);
  if (node == nodes_.end()) { return 0xc0000034; }
  if ((attributes & ~0xb7u) || ((attributes & 0x10) && !node->second.directory) ||
      ((attributes & 0x80) && attributes != 0x80)) { return 0xc000000d; }
  for (const auto time : times) {
    if ((time >> 63) && time < UINT64_MAX - 1) { return 0xc000000d; }
  }
  std::array<uint64_t*, 4> fields{&node->second.creationTime, &node->second.lastAccessTime,
    &node->second.lastWriteTime, &node->second.changeTime};
  for (size_t n = 0; n < times.size(); ++n) {
    if (times[n] && times[n] < UINT64_MAX - 1) { *fields[n] = times[n]; }
  }
  if (attributes) { node->second.attributes = (node->second.directory ? 0x10u : 0u) | (attributes & ~0x10u); }
  ++revision_;
  return 0;
}

uint32_t TemporaryFilesystem::Write(const std::string& path, uint64_t offset, std::span<const uint8_t> bytes) {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0xc0000033; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*normalized);
  if (node == nodes_.end()) { return 0xc0000034; }
  if (node->second.directory) { return 0xc00000ba; }
  if (node->second.attributes & 1) { return 0xc0000022; }
  if (bytes.empty()) { return 0; }
  if (offset > capacity_ || bytes.size() > capacity_ - offset) { return 0xc000007f; }
  const auto size = std::max<uint64_t>(node->second.bytes.size(), offset + bytes.size());
  const auto previous = std::max(Allocation(node->second.bytes.size()), node->second.reservation);
  const auto allocation = std::max(Allocation(size), node->second.reservation);
  if (allocation > capacity_ - allocated_ + previous) { return 0xc000007f; }
  const auto now = clock_();
  node->second.bytes.resize(size);
  std::copy(bytes.begin(), bytes.end(), node->second.bytes.begin() + offset);
  node->second.lastWriteTime = now;
  node->second.changeTime = now;
  allocated_ = allocated_ - previous + allocation;
  ++revision_;
  return 0;
}

size_t TemporaryFilesystem::Read(const std::string& path, uint64_t offset, std::span<uint8_t> output) const {
  const auto normalized = Normalize(path);
  if (!normalized) { return 0; }
  std::lock_guard lock(lock_);
  const auto node = nodes_.find(*normalized);
  if (node == nodes_.end() || node->second.directory || offset >= node->second.bytes.size()) { return 0; }
  const auto size = std::min<uint64_t>(output.size(), node->second.bytes.size() - offset);
  if (size) { node->second.lastAccessTime = clock_(); }
  std::copy_n(node->second.bytes.data() + offset, size, output.data());
  return size;
}

std::shared_ptr<void> TemporaryFilesystem::HoldWriter() {
  writers_->fetch_add(1, std::memory_order_acq_rel);
  return std::shared_ptr<void>(nullptr, [writers = writers_](void*) { writers->fetch_sub(1, std::memory_order_acq_rel); });
}

namespace {

constexpr std::array<uint8_t, 8> ImageMagic{'S', 'C', 'D', 'A', 'T', 'F', 'S', 'I'};
constexpr uint32_t ImageVersion = 1;
constexpr size_t NodeHeaderBytes = 1 + 4 + 4 * 8 + 8 + 8;
constexpr size_t ElidedZeros = 32;

// file bytes as chunks of a zero run and a literal run: the game preallocates its save slots as zeros
template <typename Chunk> void SparseChunks(const std::vector<uint8_t>& bytes, Chunk chunk) {
  const auto zero = [](uint8_t value) { return value == 0; };
  auto at = bytes.begin();
  while (at != bytes.end()) {
    const auto start = std::find_if_not(at, bytes.end(), zero);
    auto end = start;
    while (end != bytes.end()) {
      end = std::find(end, bytes.end(), uint8_t(0));
      const auto run = std::find_if_not(end, end + std::min<ptrdiff_t>(ElidedZeros, bytes.end() - end), zero);
      if (run == bytes.end() || run - end >= ptrdiff_t(ElidedZeros)) { break; }
      end = run;
    }
    chunk(uint32_t(start - at), start, end);
    at = end;
  }
}

}

std::vector<uint8_t> TemporaryFilesystem::Serialize(uint64_t* revision) const {
  std::lock_guard lock(lock_);
  std::vector<uint8_t> image;
  const auto put = [&](uint64_t value, size_t width) {
    for (size_t n = 0; n < width; ++n) { image.push_back(uint8_t(value >> (8 * n))); }
  };
  image.insert(image.end(), ImageMagic.begin(), ImageMagic.end());
  put(ImageVersion, 4);
  put(nodes_.size() - 1, 4);
  for (const auto& [path, node] : nodes_) {
    if (path.empty()) { continue; }
    put(path.size(), 2);
    image.insert(image.end(), path.begin(), path.end());
    put(node.directory, 1);
    put(node.attributes, 4);
    for (const auto time : {node.creationTime, node.lastAccessTime, node.lastWriteTime, node.changeTime}) { put(time, 8); }
    put(node.reservation, 8);
    put(node.bytes.size(), 8);
    SparseChunks(node.bytes, [&](uint32_t zeros, auto literal, auto end) {
      put(zeros, 4);
      put(end - literal, 4);
      image.insert(image.end(), literal, end);
    });
  }
  if (revision) { *revision = revision_.load(std::memory_order_relaxed); }
  return image;
}

uint32_t TemporaryFilesystem::Restore(std::span<const uint8_t> image) {
  constexpr uint32_t invalid = 0xc000000d;
  size_t offset = 0;
  const auto take = [&](uint64_t& value, size_t width) {
    if (image.size() - offset < width) { return false; }
    value = 0;
    for (size_t n = 0; n < width; ++n) { value |= uint64_t(image[offset + n]) << (8 * n); }
    offset += width;
    return true;
  };
  if (image.size() < ImageMagic.size() || !std::equal(ImageMagic.begin(), ImageMagic.end(), image.begin())) { return invalid; }
  offset = ImageMagic.size();
  uint64_t version = 0, count = 0;
  if (!take(version, 4) || version != ImageVersion || !take(count, 4)) { return invalid; }
  if (count >= maximumEntries_) { return 0xc000009a; }
  std::map<std::string, Node> nodes;
  nodes.emplace("", NewNode(true, 0x10));
  uint64_t allocated = AllocationUnit;
  std::string previous;
  for (uint64_t entry = 0; entry < count; ++entry) {
    uint64_t length = 0;
    if (!take(length, 2) || !length || length > 240 || image.size() - offset < length) { return invalid; }
    std::string path(reinterpret_cast<const char*>(image.data() + offset), length);
    offset += length;
    if (Normalize(path) != path || path <= previous) { return invalid; }
    const auto separator = path.rfind('\\');
    const auto parent = nodes.find(separator == std::string::npos ? "" : path.substr(0, separator));
    if (parent == nodes.end() || !parent->second.directory) { return invalid; }
    uint64_t directory = 0, attributes = 0, reservation = 0, size = 0;
    std::array<uint64_t, 4> times{};
    if (!take(directory, 1) || directory > 1 || !take(attributes, 4)) { return invalid; }
    for (auto& time : times) {
      if (!take(time, 8)) { return invalid; }
    }
    if (!take(reservation, 8) || !take(size, 8)) { return invalid; }
    if ((attributes & ~0xb7u) || bool(attributes & 0x10) != bool(directory)) { return invalid; }
    if (directory && (size || reservation)) { return invalid; }
    if (reservation % AllocationUnit || reservation > capacity_ || size > capacity_) { return 0xc000007f; }
    const auto allocation = directory ? AllocationUnit : std::max(Allocation(size), reservation);
    if (allocation > capacity_ - allocated) { return 0xc000007f; }
    std::vector<uint8_t> bytes(size);
    for (uint64_t filled = 0; filled < size;) {
      uint64_t zeros = 0, literal = 0;
      if (!take(zeros, 4) || !take(literal, 4) || !(zeros + literal) || zeros + literal > size - filled ||
          image.size() - offset < literal) { return invalid; }
      std::copy_n(image.begin() + offset, literal, bytes.begin() + filled + zeros);
      offset += literal;
      filled += zeros + literal;
    }
    Node node{bool(directory), std::move(bytes), uint32_t(attributes), times[0], times[1], times[2], times[3], reservation};
    allocated += allocation;
    nodes.emplace(path, std::move(node));
    previous = std::move(path);
  }
  if (offset != image.size()) { return invalid; }
  std::lock_guard lock(lock_);
  nodes_.swap(nodes);
  allocated_ = allocated;
  ++revision_;
  return 0;
}

}
