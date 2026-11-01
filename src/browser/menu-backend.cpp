#include "browser/menu-backend.h"
#include "browser/menu-shaders.h"
#include "browser/menu-texture.h"
#include "browser/render-snapshot.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace browser {
namespace {

std::atomic<uint32_t> resourceSequence{0};

const auto require = [](bool valid, const char* reason) {
  if (!valid) { throw std::runtime_error(reason); }
};
uint32_t address(uint32_t value) {
  return value && value < 0x04000000 ? value | 0x80000000 : value;
}
template<class T> void append(std::vector<uint8_t>& bytes, const T& value) {
  const auto* first = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), first, first + sizeof(value));
}
void appendBytes(std::vector<uint8_t>& bytes, std::span<const uint8_t> value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}
uint32_t word(std::span<const uint8_t> bytes, size_t offset) {
  require(offset <= bytes.size() && bytes.size() - offset >= 4, "menu backend short resource header");
  uint32_t result;
  std::memcpy(&result, bytes.data() + offset, 4);
  return result;
}
uint32_t typeBytes(uint32_t type) {
  if (type == 0x02) { return 0; }
  if (type == 0x72) { return 12; }
  const uint32_t count = type >> 4;
  require(count >= 1 && count <= 4, "menu backend unsupported vertex format component count");
  switch (type & 15) {
    case 0: require(type == 0x40, "menu backend unsupported vertex color format"); return 4;
    case 1: case 5: return count * 2;
    case 2: return count * 4;
    case 4: return count;
    case 6: require(type == 0x16, "menu backend unsupported packed vertex format"); return 4;
    default: throw std::runtime_error("menu backend unsupported vertex format");
  }
}
void convert(uint32_t type, const uint8_t* bytes, float* output) {
  if (type == 0x72) {
    std::memcpy(output, bytes, 8);
    std::memcpy(output + 3, bytes + 8, 4);
    return;
  }
  const uint32_t count = type >> 4;
  switch (type & 15) {
    case 0:
      output[0] = bytes[2] / 255.0f; output[1] = bytes[1] / 255.0f;
      output[2] = bytes[0] / 255.0f; output[3] = bytes[3] / 255.0f;
      break;
    case 1: case 5:
      for (uint32_t n = 0; n < count; ++n) {
        int16_t value;
        std::memcpy(&value, bytes + n * 2, 2);
        output[n] = (type & 15) == 1 ? std::max(value / 32767.0f, -1.0f) : float(value);
      }
      break;
    case 2: std::memcpy(output, bytes, count * 4); break;
    case 4:
      for (uint32_t n = 0; n < count; ++n) { output[n] = bytes[n] / 255.0f; }
      break;
    case 6: {
      uint32_t value;
      std::memcpy(&value, bytes, 4);
      output[0] = std::max((int32_t(value << 21) >> 21) / 1023.0f, -1.0f);
      output[1] = std::max((int32_t(value << 10) >> 21) / 1023.0f, -1.0f);
      output[2] = std::max((int32_t(value) >> 22) / 511.0f, -1.0f);
      break;
    }
  }
}
gpu::VertexShader layoutOf(const gpu::State& state) {
  if (state.vs_handle & 1) {
    require(bool(state.vs), "menu backend missing programmable vertex layout");
    return *state.vs;
  }
  gpu::VertexShader layout;
  uint32_t offset = 0;
  const auto add = [&](uint32_t reg, uint32_t type) {
    layout.inputs[reg] = {0, offset, type};
    offset += typeBytes(type);
  };
  const uint32_t fvf = state.vs_handle;
  switch (fvf & 14) {
    case 2: add(0, 0x32); break;
    case 4: add(0, 0x42); break;
    case 6: case 8: case 10: case 12:
      add(0, 0x32); add(1, 2 | (((fvf & 14) - 4) / 2 << 4)); break;
    default: throw std::runtime_error("menu backend unsupported FVF position layout");
  }
  if (fvf & 0x10) { add(2, 0x32); }
  if (fvf & 0x20) { add(6, 0x12); }
  if (fvf & 0x40) { add(3, 0x40); }
  if (fvf & 0x80) { add(4, 0x40); }
  const uint32_t textures = (fvf >> 8) & 15;
  require(textures <= 4, "menu backend unsupported FVF texture coordinate count");
  constexpr uint32_t types[] = {0x22, 0x32, 0x42, 0x12};
  for (uint32_t n = 0; n < textures; ++n) { add(9 + n, types[(fvf >> (16 + n * 2)) & 3]); }
  layout.stream_stride[0] = offset;
  return layout;
}
std::vector<uint32_t> orderOf(uint32_t primitive, uint32_t count) {
  require(primitive >= 1 && primitive <= 10, "menu backend unsupported topology");
  std::vector<uint32_t> order;
  order.reserve(size_t(count) * 3);
  for (uint32_t n = 0; n < count; ++n) {
    switch (primitive) {
      case 1: order.push_back(n); break;
      case 2: if (n % 2 == 0 && n + 1 < count) { order.insert(order.end(), {n, n + 1}); } break;
      case 3: case 4:
        if (n + 1 < count) { order.insert(order.end(), {n, n + 1}); }
        if (primitive == 3 && n + 1 == count && count > 2) { order.insert(order.end(), {n, 0}); }
        break;
      case 5: if (n % 3 == 0 && n + 2 < count) { order.insert(order.end(), {n, n + 1, n + 2}); } break;
      case 6:
        if (n + 2 < count) {
          if (n & 1) { order.insert(order.end(), {n + 1, n, n + 2}); }
          else { order.insert(order.end(), {n, n + 1, n + 2}); }
        }
        break;
      case 7: case 10: if (n > 0 && n + 1 < count) { order.insert(order.end(), {0, n, n + 1}); } break;
      case 8: if (n % 4 == 0 && n + 3 < count) { order.insert(order.end(), {n, n + 1, n + 2, n, n + 2, n + 3}); } break;
      case 9: if (n % 2 == 0 && n + 3 < count) { order.insert(order.end(), {n, n + 1, n + 2, n + 1, n + 3, n + 2}); } break;
    }
  }
  return order;
}
uint32_t blendFactor(uint32_t value, bool alpha) {
  switch (value) {
    case 0: return 1;
    case 1: return 2;
    case 0x300: return alpha ? 5 : 3;
    case 0x301: return alpha ? 6 : 4;
    case 0x302: return 5;
    case 0x303: return 6;
    case 0x304: return 9;
    case 0x305: return 10;
    case 0x306: return alpha ? 9 : 7;
    case 0x307: return alpha ? 10 : 8;
    default: throw std::runtime_error("menu backend unsupported blend factor");
  }
}
uint32_t blendOperation(uint32_t value) {
  switch (value) {
    case 0x8006: return 1;
    case 0x800a: return 2;
    case 0x800b: return 3;
    case 0x8007: return 4;
    case 0x8008: return 5;
    default: throw std::runtime_error("menu backend unsupported blend operation");
  }
}
uint32_t samplerAddress(uint32_t value) {
  switch (value) {
    case 1: return 2;
    case 2: return 3;
    case 3: return 1;
    default: throw std::runtime_error("menu backend unsupported sampler address mode");
  }
}
}

struct MenuBackend::Impl {
  struct Resource {
    uint32_t id = 0;
    uint64_t version = 1;
    uint32_t command = 0;
    gpu::TextureDesc desc;
    std::vector<uint8_t> header;
    std::set<uint32_t> views;
    std::vector<uint8_t> uploaded;
    std::vector<uint8_t> late;
    size_t residentBytes = 0;
    bool depth = false;
    bool dirty = false;
    uint64_t lateVersion = 0;
  };
  Runtime& runtime;
  PreparedImage& image;
  Memory* memory;
  Execute execute;
  MenuBackendLimits limits;
  mutable std::recursive_mutex mutex;
  std::map<uint32_t, Resource> resources;
  std::map<std::vector<uint8_t>, uint32_t> pipelines;
  std::map<uint32_t, std::pair<uint32_t, uint64_t>> uploadVersions;
  uint32_t white = 0;
  uint32_t width = 0, height = 0;
  uint32_t submitted = 0, completed = 0;
  uint64_t submittedCount = 0, completedCount = 0;
  uint32_t nextFence = 0, completedFence = 0;
  uint64_t frames = 0, finishedFrames = 0;
  size_t resident = 0;
  std::map<uint32_t, uint32_t> fences;
  bool initialized = false;

  Impl(Runtime& owner, PreparedImage& prepared, Execute exchange, MenuBackendLimits budget)
    : runtime(owner), image(prepared), memory(prepared.memory.get()), execute(std::move(exchange)), limits(budget) {
    require(memory && execute && (!image.boundRuntime || image.boundRuntime == &runtime), "menu backend requires its runtime and prepared image");
  }
  void requireOwner() const {
    require(memory == image.memory.get() && (!image.boundRuntime || image.boundRuntime == &runtime), "menu backend foreign prepared image or runtime");
    require(!Memory::HasBorrow(), "menu backend cannot wait with borrowed guest memory");
    runtime.Checkpoint({});
  }
  webgpu::Result send(webgpu::Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes = {}) {
    requireOwner();
    require(bytes.size() <= webgpu::MaxBytes, "menu backend command payload budget exceeded");
    auto result = execute(opcode, resource, bytes);
    if (result.status != webgpu::Status::Ok) {
      throw std::runtime_error("menu backend GPU service failure: " + std::string(result.bytes.begin(), result.bytes.end()));
    }
    require(result.resource == resource && result.command > submitted, "menu backend GPU service invalid completion identity");
    submitted = result.command;
    require(submittedCount != UINT64_MAX, "menu backend command counter budget exceeded");
    ++submittedCount;
    if (opcode == webgpu::Opcode::Flush || opcode == webgpu::Opcode::Readback) {
      completed = submitted; completedCount = submittedCount; finishedFrames = frames;
      for (auto found = fences.begin(); found != fences.end();) {
        if (found->second > completed) { break; }
        completedFence = found->first; found = fences.erase(found);
      }
    }
    return result;
  }
  template<class T> webgpu::Result sendPacket(webgpu::Opcode opcode, uint32_t resource, const T& packet) {
    std::vector<uint8_t> bytes;
    append(bytes, packet);
    return send(opcode, resource, bytes);
  }
  uint32_t newId() {
    uint32_t previous = resourceSequence.load();
    while (true) {
      require(previous != UINT32_MAX, "menu backend resource ID budget exhausted");
      if (resourceSequence.compare_exchange_weak(previous, previous + 1)) { return previous + 1; }
    }
  }
  std::vector<uint8_t> read(uint32_t location, uint64_t count) {
    require(count && count <= limits.maxResourceBytes, "menu backend resource byte budget exceeded");
    require(uint64_t(location) + count <= uint64_t(UINT32_MAX) + 1, "menu backend guest memory range overflow");
    std::vector<uint8_t> bytes(size_t(count), 0);
    memory->Read({}, location, bytes.data(), bytes.size());
    return bytes;
  }
  gpu::TextureDesc describe(const std::vector<uint8_t>& header, bool surface) {
    const uint32_t common = word(header, 0), format = word(header, 12), size = word(header, 16);
    require((common & 0x70000) == (surface ? 0x50000u : 0x40000u), "menu backend incompatible resource header format");
    gpu::TextureDesc desc;
    desc.data = address(word(header, 4));
    desc.format = (format >> 8) & 255;
    desc.cube = (format & 4) != 0;
    desc.levels = std::max((format >> 16) & 15, 1u);
    if (size) {
      desc.width = (size & 4095) + 1; desc.height = ((size >> 12) & 4095) + 1;
      desc.pitch = ((size >> 24) + 1) * 64; desc.depth = 1; desc.levels = 1; desc.linear = true;
    } else {
      desc.width = 1u << ((format >> 20) & 15); desc.height = 1u << ((format >> 24) & 15);
      desc.depth = ((format >> 4) & 15) == 3 ? 1u << ((format >> 28) & 15) : 1;
      switch (desc.format) {
        case 0x13: case 0x1b: case 0x1f:
          desc.linear = true; desc.pitch = desc.width; break;
        case 0x10: case 0x11: case 0x16: case 0x17: case 0x1c: case 0x1d: case 0x20:
        case 0x30: case 0x31: case 0x35: case 0x37: case 0x3d: case 0x3e:
          desc.linear = true; desc.pitch = desc.width * 2; break;
        case 0x12: case 0x1e: case 0x2e: case 0x2f: case 0x36: case 0x3f: case 0x40: case 0x41:
          desc.linear = true; desc.pitch = desc.width * 4; break;
        default: desc.linear = false; break;
      }
    }
    require(!desc.cube && desc.depth == 1 && desc.levels == 1, "menu backend unsupported texture dimension or mip chain");
    require(desc.width <= 4096 && desc.height <= 4096, "menu backend texture dimension budget exceeded");
    return desc;
  }
  uint64_t resourceBytes(const gpu::TextureDesc& desc, bool depth) {
    if (depth) {
      require(desc.linear && (desc.format == 0x2e || desc.format == 0x30), "menu backend unsupported depth surface format");
      return uint64_t(desc.pitch) * desc.height;
    }
    auto layout = menu::DescribeTextureLayout(desc);
    require(layout.supported, layout.diagnostic.c_str());
    return layout.total_bytes;
  }
  Resource& ensure(uint32_t headerAddress, bool surface, bool depth = false) {
    require(headerAddress, "menu backend missing target resource");
    auto header = read(headerAddress, 20);
    auto desc = describe(header, surface);
    if (surface && !depth) {
      require(desc.linear && (desc.format == 0x12 || desc.format == 0x1e), "menu backend unsupported color surface format");
    }
    const uint64_t bytes = resourceBytes(desc, depth);
    require(bytes <= limits.maxResourceBytes, "menu backend resource byte budget exceeded");
    auto found = resources.find(desc.data);
    for (const auto& [location, item] : resources) {
      if (location != desc.data) {
        require(uint64_t(desc.data) + bytes <= item.desc.data || uint64_t(item.desc.data) + item.uploaded.size() <= desc.data, "menu backend unsupported overlapping GPU resource view");
      }
    }
    if (found != resources.end()) {
      auto& item = found->second;
      require(item.depth == depth && item.desc.format == desc.format && item.desc.width == desc.width && item.desc.height == desc.height && item.desc.pitch == desc.pitch && item.desc.linear == desc.linear && item.uploaded.size() == bytes, "menu backend unsupported incompatible GPU storage view");
      require(item.views.contains(headerAddress) || item.views.size() < limits.maxResources, "menu backend resource view count budget exceeded");
      item.views.insert(headerAddress); item.header = header;
      if (item.dirty || depth) { return item; }
      auto source = read(desc.data, bytes);
      if (source == item.uploaded) { return item; }
    }
    auto source = read(desc.data, bytes);
    std::vector<uint8_t> rgba;
    if (!depth) {
      auto decoded = menu::DecodeTexture(desc, source);
      require(decoded.supported, decoded.diagnostic.c_str());
      rgba = std::move(decoded.pixels);
    }
    const size_t residentBytes = size_t(bytes) + size_t(desc.width) * desc.height * 4;
    const size_t oldBytes = found == resources.end() ? 0 : found->second.residentBytes + found->second.late.size();
    require(resources.size() + size_t(white != 0) < limits.maxResources || found != resources.end(), "menu backend resource count budget exceeded");
    require(residentBytes <= limits.maxResidentBytes && resident - std::min(resident, oldBytes) <= limits.maxResidentBytes - residentBytes, "menu backend resident byte budget exceeded");
    const uint32_t id = newId();
    webgpu::TexturePacket packet{desc.width, desc.height, uint32_t(depth)};
    std::vector<uint8_t> payload;
    append(payload, packet); appendBytes(payload, rgba);
    const auto result = send(webgpu::Opcode::CreateTexture, id, payload);
    uint64_t version = 1;
    if (found != resources.end()) {
      version = found->second.version + 1;
      send(webgpu::Opcode::Destroy, found->second.id);
      resident -= std::min(resident, oldBytes);
    }
    Resource item;
    item.id = id; item.version = version; item.command = result.command;
    item.desc = desc; item.header = std::move(header); item.uploaded = std::move(source); item.depth = depth; item.residentBytes = residentBytes;
    if (found != resources.end()) { item.views = std::move(found->second.views); }
    item.views.insert(headerAddress);
    resources[desc.data] = std::move(item);
    resident += residentBytes;
    return resources.at(desc.data);
  }
  Resource& surface(const gpu::Surface& value, bool depth = false) {
    auto& item = ensure(value.header, true, depth);
    require(item.desc.data == address(value.data) && word(item.header, 12) == value.format && word(item.header, 16) == value.size, "menu backend stale or incompatible surface descriptor");
    return item;
  }
  uint32_t whiteTexture() {
    if (!white) {
      require(resources.size() < limits.maxResources, "menu backend resource count budget exceeded");
      require(limits.maxResidentBytes >= 8 && resident <= limits.maxResidentBytes - 8, "menu backend white texture resident budget exceeded");
      const webgpu::TexturePacket packet{1, 1, 0};
      std::vector<uint8_t> bytes;
      append(bytes, packet); bytes.insert(bytes.end(), {255, 255, 255, 255});
      const uint32_t id = newId();
      send(webgpu::Opcode::CreateTexture, id, bytes);
      white = id; resident += 8;
    }
    return white;
  }
  Resource& byAddress(uint32_t location) {
    location = address(location);
    for (auto& [header, item] : resources) {
      if (location >= item.desc.data && uint64_t(location) < uint64_t(item.desc.data) + item.uploaded.size()) { return item; }
    }
    throw std::runtime_error("menu backend readback unknown resource");
  }
  bool readHeader(const Resource& item) {
    for (uint32_t location : item.views) {
      auto header = read(location, 20);
      if (address(word(header, 4)) == item.desc.data && word(header, 12) == word(item.header, 12) && word(header, 16) == word(item.header, 16)) { return true; }
    }
    return false;
  }
  std::vector<uint8_t> readback(Resource& item) {
    require(!item.depth && item.desc.linear && (item.desc.format == 0x12 || item.desc.format == 0x1e), "menu backend unsupported readback resource format");
    require(readHeader(item), "menu backend stale resource header before readback");
    const auto result = send(webgpu::Opcode::Readback, item.id);
    require(result.bytes.size() == uint64_t(item.desc.width) * item.desc.height * 4, "menu backend GPU service invalid readback byte count");
    auto bytes = item.uploaded;
    for (uint32_t n = 0; n < item.desc.height; ++n) {
      for (uint32_t x = 0; x < item.desc.width; ++x) {
        const size_t source = (size_t(n) * item.desc.width + x) * 4;
        const size_t destination = size_t(n) * item.desc.pitch + x * 4;
        require(destination + 4 <= bytes.size(), "menu backend readback pitch exceeds resource");
        bytes[destination] = result.bytes[source + 2]; bytes[destination + 1] = result.bytes[source + 1];
        bytes[destination + 2] = result.bytes[source]; bytes[destination + 3] = item.desc.format == 0x1e ? 255 : result.bytes[source + 3];
      }
    }
    return bytes;
  }
  void checkState(const gpu::State& state) {
    require(!state.rs[gpu::kRsStencilEnable], "menu backend unsupported stencil draw state");
    require(!state.rs[gpu::kRsLogicOp], "menu backend unsupported logic operation");
    require(!state.rs[gpu::kRsZBias] && !state.simple[gpu::kMSolidOffsetEnable / 4], "menu backend unsupported depth bias");
    require(!state.rs[gpu::kRsEdgeAntiAlias] && !state.rs[gpu::kRsSampleAlpha], "menu backend unsupported antialias state");
    require(!state.rs[gpu::kRsPointSpriteEnable] && !state.rs[gpu::kRsPointScaleEnable], "menu backend unsupported point sprite state");
    require(state.rs[gpu::kRsFillMode] == 0 || state.rs[gpu::kRsFillMode] == 0x1b02, "menu backend unsupported fill mode");
    require(state.rs[gpu::kRsBackFillMode] == 0 || state.rs[gpu::kRsBackFillMode] == 0x1b02, "menu backend unsupported back fill mode");
    require(!state.rs[gpu::kRsMultiSampleMode] && !state.rs[gpu::kRsMultiSampleRenderTargetMode], "menu backend unsupported multisample state");
    require(state.scissor_count <= 1, "menu backend unsupported multiple scissor rectangles");
  }
};

MenuBackend::MenuBackend(Runtime& runtime, PreparedImage& image, std::shared_ptr<webgpu::Transport> transport, MenuBackendLimits limits)
  : MenuBackend(runtime, image, [transport](auto opcode, auto resource, auto bytes) {
    require(bool(transport), "menu backend missing injected GPU transport");
    return transport->Execute(opcode, resource, bytes);
  }, limits) {}
MenuBackend::MenuBackend(Runtime& runtime, PreparedImage& image, Execute execute, MenuBackendLimits limits)
  : impl_(std::make_unique<Impl>(runtime, image, std::move(execute), limits)) {}
MenuBackend::~MenuBackend() {
  try {
    std::lock_guard lock(impl_->mutex);
    for (const auto& [key, id] : impl_->pipelines) { impl_->send(webgpu::Opcode::Destroy, id); }
    for (const auto& [header, item] : impl_->resources) { impl_->send(webgpu::Opcode::Destroy, item.id); }
    if (impl_->white) { impl_->send(webgpu::Opcode::Destroy, impl_->white); }
  } catch (...) {}
}
MenuBackend::SnapshotData MenuBackend::Snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return {uint32_t(impl_->pipelines.size()), uint32_t(impl_->resources.size()) + uint32_t(impl_->white != 0), impl_->resident,
    impl_->submittedCount, impl_->completedCount, impl_->frames};
}
bool MenuBackend::Init(uint32_t width, uint32_t height) {
  std::lock_guard lock(impl_->mutex);
  require(!impl_->initialized, "menu backend already initialized");
  require(width && height && width <= 4096 && height <= 4096, "menu backend target dimension budget exceeded");
  impl_->sendPacket(webgpu::Opcode::Configure, 0, webgpu::Dimensions{width, height});
  impl_->width = width; impl_->height = height; impl_->initialized = true;
  return true;
}
void MenuBackend::Draw(const gpu::State& input, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  require(impl_->initialized, "menu backend draw before initialization");
  require(count <= impl_->limits.maxVertices, "menu backend vertex budget exceeded");
  impl_->checkState(input);
  auto order = orderOf(prim, count);
  require(order.size() <= impl_->limits.maxVertices, "menu backend expanded vertex budget exceeded");
  if (order.empty()) { return; }
  require(!input.vs || (input.vs->decl.size() <= 65536 && input.vs->program.size() <= 65536 && input.vs->constants.size() <= 192), "menu backend shader budget exceeded");
  auto layout = layoutOf(input);
  const auto& budget = impl_->limits;
  require(layout.decl.size() <= 65536 && layout.program.size() <= 65536 && layout.constants.size() <= 192, "menu backend shader budget exceeded");
  std::vector<uint16_t> indexValues;
  if (indices) {
    indexValues.resize(count);
    std::memcpy(indexValues.data(), indices, size_t(count) * 2);
  }
  std::vector<uint32_t> vertexNumbers(count);
  uint32_t maximum = 0;
  for (uint32_t n = 0; n < count; ++n) {
    const uint64_t vertex = indices ? uint64_t(indexValues[n]) + input.base_vertex : uint64_t(start) + n;
    require(vertex <= UINT32_MAX, "menu backend vertex address overflow");
    maximum = std::max(maximum, uint32_t(vertex)); vertexNumbers[n] = uint32_t(vertex);
  }
  std::array<uint32_t, 16> streamBytes{};
  uint16_t regs = 0;
  for (uint32_t n = 0; n < 16; ++n) {
    const auto& element = layout.inputs[n];
    require(element.stream >= -1 && element.stream < 16, "menu backend unsupported vertex stream");
    if (element.stream < 0 || element.type == 0 || element.type == 0x02) { continue; }
    const uint32_t size = typeBytes(element.type);
    const auto& stream = input.streams[size_t(element.stream)];
    if (!stream.vb) { continue; }
    const uint32_t stride = stream.stride ? stream.stride : layout.stream_stride[size_t(element.stream)];
    require(stride, "menu backend unsupported zero vertex stride");
    const uint64_t bytes = uint64_t(maximum) * stride + element.offset + size;
    require(bytes <= budget.maxResourceBytes, "menu backend vertex resource budget exceeded");
    streamBytes[size_t(element.stream)] = std::max(streamBytes[size_t(element.stream)], uint32_t(bytes));
    regs |= uint16_t(1u << n);
  }
  require(regs, "menu backend missing vertex inputs");
  std::vector<RenderResourceRequest> requests;
  std::map<uint32_t, size_t> requestIndex;
  const auto addRequest = [&](RenderResourceKind kind, uint32_t header, uint32_t data, uint32_t bytes, uint64_t version, uint32_t gpuId = 0) {
    if (header && requestIndex.contains(header)) { return; }
    if (header) { requestIndex[header] = requests.size(); }
    uint32_t id = 0;
    if (kind == RenderResourceKind::Texture || kind == RenderResourceKind::Surface) {
      require(gpuId, "menu backend missing snapshot GPU storage identity");
      id = gpuId;
    } else {
      const uint32_t identity = header ? header : data;
      auto found = impl_->uploadVersions.find(identity);
      if (found == impl_->uploadVersions.end()) {
        require(impl_->uploadVersions.size() < budget.maxResources, "menu backend upload resource count budget exceeded");
        found = impl_->uploadVersions.emplace(identity, std::pair{impl_->newId(), uint64_t(0)}).first;
      }
      require(found->second.second != UINT64_MAX, "menu backend upload version budget exceeded");
      id = found->second.first; version = ++found->second.second;
    }
    requests.push_back({id, version, kind, header, data, bytes});
  };
  for (uint32_t n = 0; n < 16; ++n) {
    const auto& stream = input.streams[n];
    if (!stream.vb) { continue; }
    auto header = impl_->read(stream.vb, 12);
    require((word(header, 0) & 0x70000) == 0, "menu backend incompatible vertex buffer format");
    addRequest(RenderResourceKind::VertexBuffer, stream.vb, address(word(header, 4)), std::max(streamBytes[n], 1u), 1);
  }
  gpu::TexInfo texinfo[4]{};
  std::array<std::array<float, 4>, 4> texsize{};
  webgpu::DrawPacket packet{};
  for (uint32_t n = 0; n < 4; ++n) {
    if (input.textures[n]) {
      auto& item = impl_->ensure(input.textures[n], false);
      require(item.desc.data != address(input.rt.data), "menu backend unsupported render target feedback sampling");
      texinfo[n].kind = gpu::kTex2D; texinfo[n].rect = item.desc.linear;
      texsize[n] = {float(item.desc.width), float(item.desc.height), 1, 0};
      packet.texture[n] = item.id;
      addRequest(RenderResourceKind::Texture, input.textures[n], item.desc.data, uint32_t(item.uploaded.size()), item.version, item.id);
      const auto& sampling = input.tss[n];
      require(!sampling[gpu::kTssMipLodBias] && !sampling[gpu::kTssMaxMipLevel] && sampling[gpu::kTssMaxAnisotropy] <= 1, "menu backend unsupported sampler mip or anisotropy state");
      packet.sampler[n][0] = samplerAddress(sampling[gpu::kTssAddressU]);
      packet.sampler[n][1] = samplerAddress(sampling[gpu::kTssAddressV]);
      const uint32_t mag = sampling[gpu::kTssMagFilter], min = sampling[gpu::kTssMinFilter], mip = sampling[gpu::kTssMipFilter];
      require(mag >= 1 && mag <= 2 && min >= 1 && min <= 2 && mip <= 2, "menu backend unsupported sampler filter");
      packet.sampler[n][2] = mag; packet.sampler[n][3] = min; packet.sampler[n][4] = mip == 2 ? 2 : 1;
    } else {
      packet.texture[n] = impl_->whiteTexture();
      std::fill(std::begin(packet.sampler[n]), std::end(packet.sampler[n]), 1);
    }
  }
  auto& target = impl_->surface(input.rt);
  addRequest(RenderResourceKind::Surface, input.rt.header, target.desc.data, uint32_t(target.uploaded.size()), target.version, target.id);
  if (input.zs.header) {
    auto& depth = impl_->surface(input.zs, true);
    require(depth.desc.width == target.desc.width && depth.desc.height == target.desc.height, "menu backend incompatible depth target dimensions");
    packet.depth_target = depth.id;
    addRequest(RenderResourceKind::Surface, input.zs.header, depth.desc.data, uint32_t(depth.uploaded.size()), depth.version, depth.id);
  }
  if (input.index_buffer) {
    auto header = impl_->read(input.index_buffer, 12);
    const uint32_t data = address(word(header, 4));
    require(!input.index_data || address(input.index_data) == data, "menu backend incompatible index buffer data binding");
    addRequest(RenderResourceKind::IndexBuffer, input.index_buffer, data, indices ? count * 2 : 1, 1);
  } else if (input.index_data) {
    addRequest(RenderResourceKind::IndexData, 0, input.index_data, indices ? count * 2 : 1, 1);
  }
  RenderSnapshotLimits snapshotLimits;
  snapshotLimits.maxResources = budget.maxResources; snapshotLimits.maxPayloadBytes = budget.maxResourceBytes;
  snapshotLimits.maxTotalBytes = budget.maxResidentBytes;
  auto snapshot = CaptureRenderSnapshot({}, *impl_->memory, input, requests, snapshotLimits);
  auto& state = snapshot.state;
  if (snapshot.vertexShader) { state.vs = std::make_shared<gpu::VertexShader>(*snapshot.vertexShader); }
  if (snapshot.pixelShader) { state.ps = std::make_shared<gpu::PixelShader>(*snapshot.pixelShader); }
  const bool pretransformed = !(state.vs_handle & 1) && (state.vs_handle & 14) == 4;
  const auto shaders = menu::GenerateShaders(state, regs, pretransformed, texinfo);
  require(shaders.supported, shaders.diagnostic.c_str());
  webgpu::PipelinePacket pipeline{};
  pipeline.vertex_bytes = uint32_t(shaders.vertex.size()); pipeline.fragment_bytes = uint32_t(shaders.fragment.size());
  pipeline.topology = prim == 1 ? 1 : prim <= 4 ? 2 : 4;
  pipeline.cull = 1; pipeline.front_face = 1;
  const uint32_t cull = state.rs[gpu::kRsCullMode];
  require(cull == 0 || cull == 0x900 || cull == 0x901, "menu backend unsupported cull mode");
  if (cull && prim >= 5) { pipeline.cull = 3; pipeline.front_face = cull == 0x900 ? 1 : 2; }
  pipeline.blend = state.simple[gpu::kMBlendEnable / 4] != 0;
  pipeline.src_color = pipeline.src_alpha = 2; pipeline.dst_color = pipeline.dst_alpha = 1;
  pipeline.color_op = pipeline.alpha_op = 1;
  if (pipeline.blend) {
    pipeline.src_color = blendFactor(state.simple[gpu::kMSrcBlend / 4], false);
    pipeline.dst_color = blendFactor(state.simple[gpu::kMDestBlend / 4], false);
    pipeline.src_alpha = blendFactor(state.simple[gpu::kMSrcBlend / 4], true);
    pipeline.dst_alpha = blendFactor(state.simple[gpu::kMDestBlend / 4], true);
    pipeline.color_op = pipeline.alpha_op = blendOperation(state.simple[gpu::kMBlendOp / 4]);
  }
  const uint32_t mask = state.simple[gpu::kMColorWriteEnable / 4];
  pipeline.write_mask = ((mask >> 16) & 1) | ((mask >> 8) & 1) << 1 | (mask & 1) << 2 | ((mask >> 24) & 1) << 3;
  pipeline.depth = packet.depth_target != 0;
  pipeline.depth_compare = 8;
  if (state.rs[gpu::kRsZEnable]) {
    require(pipeline.depth, "menu backend depth enabled without target");
    const uint32_t compare = state.simple[gpu::kMZFunc / 4];
    require(compare >= 0x200 && compare <= 0x207, "menu backend unsupported depth comparison");
    pipeline.depth_compare = compare - 0x200 + 1;
    pipeline.depth_write = state.simple[gpu::kMZWriteEnable / 4] != 0;
  }
  std::vector<uint8_t> key;
  append(key, pipeline); append(key, regs); append(key, prim);
  for (const auto& element : layout.inputs) { append(key, element.stream); append(key, element.offset); append(key, element.type); }
  for (const auto& stream : state.streams) { append(key, stream.stride); }
  append(key, layout.stream_stride); append(key, state.vs_handle);
  append(key, state.rt.format); append(key, state.rt.size); append(key, state.zs.format);
  key.insert(key.end(), shaders.vertex.begin(), shaders.vertex.end()); key.insert(key.end(), shaders.fragment.begin(), shaders.fragment.end());
  auto foundPipeline = impl_->pipelines.find(key);
  if (foundPipeline == impl_->pipelines.end()) {
    require(impl_->pipelines.size() < budget.maxPipelines, "menu backend pipeline count budget exceeded");
    std::vector<uint8_t> bytes;
    append(bytes, pipeline); bytes.insert(bytes.end(), shaders.vertex.begin(), shaders.vertex.end()); bytes.insert(bytes.end(), shaders.fragment.begin(), shaders.fragment.end());
    const uint32_t id = impl_->newId();
    impl_->send(webgpu::Opcode::CreatePipeline, id, bytes);
    foundPipeline = impl_->pipelines.emplace(std::move(key), id).first;
  }
  std::vector<float> vertices(order.size() * 64, 0);
  for (uint32_t n = 0; n < order.size(); ++n) {
    for (uint32_t reg = 0; reg < 16; ++reg) {
      auto* output = vertices.data() + size_t(n) * 64 + reg * 4;
      output[3] = 1;
      if (!(regs & (1u << reg))) { continue; }
      const auto& element = layout.inputs[reg];
      const auto& stream = state.streams[size_t(element.stream)];
      const uint32_t stride = stream.stride ? stream.stride : layout.stream_stride[size_t(element.stream)];
      const auto& resource = snapshot.resources.at(requestIndex.at(stream.vb));
      const uint64_t offset = uint64_t(vertexNumbers[order[n]]) * stride + element.offset;
      require(offset + typeBytes(element.type) <= resource.payload.size(), "menu backend vertex exceeds owned snapshot");
      convert(element.type, resource.payload.data() + offset, output);
    }
  }
  packet.pipeline = foundPipeline->second; packet.vertex_count = uint32_t(order.size()); packet.vertex_bytes = uint32_t(vertices.size() * 4);
  const auto constants = menu::BuildConstants(state, target.desc.width, target.desc.height, texsize);
  packet.uniform_bytes = sizeof(constants);
  require(uint64_t(state.viewport.x) + state.viewport.w <= target.desc.width && uint64_t(state.viewport.y) + state.viewport.h <= target.desc.height && state.viewport.w && state.viewport.h, "menu backend unsupported viewport outside target");
  require(std::isfinite(state.viewport.min_z) && std::isfinite(state.viewport.max_z) && state.viewport.min_z >= 0 && state.viewport.max_z <= 1 && state.viewport.min_z <= state.viewport.max_z, "menu backend unsupported viewport depth range");
  packet.viewport[0] = float(state.viewport.x); packet.viewport[1] = float(state.viewport.y);
  packet.viewport[2] = float(state.viewport.w); packet.viewport[3] = float(state.viewport.h);
  packet.viewport[4] = state.viewport.min_z; packet.viewport[5] = state.viewport.max_z;
  packet.scissor[2] = target.desc.width; packet.scissor[3] = target.desc.height;
  if (state.scissor_count) {
    require(state.scissors[0] <= state.scissors[2] && state.scissors[1] <= state.scissors[3] && state.scissors[2] <= target.desc.width && state.scissors[3] <= target.desc.height, "menu backend unsupported scissor outside target");
    packet.scissor[0] = state.scissors[0]; packet.scissor[1] = state.scissors[1];
    packet.scissor[2] = state.scissors[2] - state.scissors[0]; packet.scissor[3] = state.scissors[3] - state.scissors[1];
  }
  std::vector<uint8_t> bytes;
  append(bytes, packet);
  appendBytes(bytes, {reinterpret_cast<const uint8_t*>(vertices.data()), vertices.size() * sizeof(float)});
  append(bytes, constants);
  const auto result = impl_->send(webgpu::Opcode::Draw, target.id, bytes);
  target.dirty = true; target.command = result.command; ++target.version;
  if (state.zs.header) {
    auto& depth = impl_->resources.at(address(state.zs.data));
    depth.dirty = true; depth.command = result.command; ++depth.version;
  }
}
void MenuBackend::Clear(const gpu::State& state, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
  std::lock_guard lock(impl_->mutex);
  require(impl_->initialized, "menu backend clear before initialization");
  require(!count && !rects, "menu backend unsupported partial clear rectangles");
  require((flags & ~(gpu::kClearTarget | gpu::kClearZ | gpu::kClearStencil)) == 0 && (!(flags & gpu::kClearTarget) || (flags & gpu::kClearTarget) == gpu::kClearTarget), "menu backend unsupported clear flags or color channels");
  if (!flags) { return; }
  require(std::isfinite(z) && z >= 0 && z <= 1, "menu backend unsupported clear depth");
  webgpu::ClearPacket packet{};
  packet.color[0] = ((color >> 16) & 255) / 255.0f; packet.color[1] = ((color >> 8) & 255) / 255.0f;
  packet.color[2] = (color & 255) / 255.0f; packet.color[3] = (color >> 24) / 255.0f;
  packet.depth = z; packet.stencil = stencil;
  uint32_t target = 0;
  if (flags & gpu::kClearTarget) { target = impl_->surface(state.rt).id; packet.flags |= 1; }
  if (flags & (gpu::kClearZ | gpu::kClearStencil)) { packet.depth_target = impl_->surface(state.zs, true).id; }
  if (flags & gpu::kClearZ) { packet.flags |= 2; }
  if (flags & gpu::kClearStencil) { packet.flags |= 4; }
  const auto result = impl_->sendPacket(webgpu::Opcode::Clear, target, packet);
  for (auto& [header, item] : impl_->resources) {
    if (item.id == target || item.id == packet.depth_target) { item.dirty = true; item.command = result.command; ++item.version; }
  }
}
void MenuBackend::Copy(const gpu::Surface& source, const gpu::Surface& destination, uint32_t count, uint32_t sourceRects, uint32_t destinationPoints) {
  std::lock_guard lock(impl_->mutex);
  require(impl_->initialized, "menu backend copy before initialization");
  require(!count && !sourceRects && !destinationPoints, "menu backend unsupported partial copy rectangles");
  auto& from = impl_->surface(source); auto& to = impl_->surface(destination);
  require(from.id != to.id && from.desc.format == to.desc.format && from.desc.width == to.desc.width && from.desc.height == to.desc.height, "menu backend incompatible surface copy");
  const auto result = impl_->sendPacket(webgpu::Opcode::Copy, to.id, webgpu::CopyPacket{from.id, from.desc.width, from.desc.height});
  to.dirty = true; to.command = result.command; ++to.version;
}
void MenuBackend::Present(const gpu::Surface& front, bool pillarbox) {
  std::lock_guard lock(impl_->mutex);
  require(impl_->initialized, "menu backend present before initialization");
  auto& item = impl_->surface(front);
  const auto result = impl_->sendPacket(webgpu::Opcode::Present, item.id, webgpu::PresentPacket{uint32_t(pillarbox)});
  item.command = result.command; ++impl_->frames;
}
void MenuBackend::Sync(uint32_t location, uint32_t size) {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  auto& item = impl_->byAddress(location);
  if (!size) { size = uint32_t(item.uploaded.size()) - (address(location) - item.desc.data); }
  require(uint64_t(address(location)) + size <= uint64_t(item.desc.data) + item.uploaded.size(), "menu backend readback range exceeds resource");
  auto bytes = impl_->readback(item);
  const size_t offset = address(location) - item.desc.data;
  impl_->memory->Write({}, address(location), bytes.data() + offset, size);
  std::copy_n(bytes.data() + offset, size, item.uploaded.data() + offset);
  if (offset == 0 && size == item.uploaded.size()) { item.dirty = false; }
}
void MenuBackend::RecordLate(uint32_t location) {
  std::lock_guard lock(impl_->mutex);
  auto& item = impl_->byAddress(location);
  const size_t extra = item.uploaded.size();
  require(extra <= impl_->limits.maxResidentBytes && impl_->resident - item.late.size() <= impl_->limits.maxResidentBytes - extra, "menu backend late readback resident budget exceeded");
  auto bytes = impl_->readback(item);
  impl_->resident = impl_->resident - item.late.size() + bytes.size();
  item.late = std::move(bytes); item.lateVersion = item.version;
}
bool MenuBackend::DeliverLate(uint32_t location) {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  auto& item = impl_->byAddress(location);
  if (item.late.empty()) { return false; }
  require(impl_->readHeader(item), "menu backend stale late readback resource header");
  impl_->memory->Write({}, item.desc.data, item.late.data(), item.late.size());
  impl_->resident -= item.late.size();
  item.uploaded = std::move(item.late);
  item.dirty = item.version != item.lateVersion;
  item.late.clear();
  return true;
}
void MenuBackend::SyncLate(uint32_t location) {
  if (!DeliverLate(location)) { Sync(location, 0); }
  RecordLate(location);
}
void MenuBackend::Flush() { std::lock_guard lock(impl_->mutex); impl_->send(webgpu::Opcode::Flush, 0); }
void MenuBackend::SetGamma(const uint8_t* ramp) {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  require(ramp, "menu backend missing gamma ramp");
  std::array<uint8_t, 768> bytes;
  std::memcpy(bytes.data(), ramp, bytes.size());
  impl_->send(webgpu::Opcode::SetGamma, 0, bytes);
}
void MenuBackend::Occlusion(const gpu::State&, const float*) { throw std::runtime_error("menu backend unsupported occlusion effect"); }
void MenuBackend::FilterShadows(const gpu::State&, const float*) { throw std::runtime_error("menu backend unsupported shadow filtering"); }
void MenuBackend::ApplyShadows(const gpu::State&) { throw std::runtime_error("menu backend unsupported shadow application"); }
uint32_t MenuBackend::Fence() {
  std::lock_guard lock(impl_->mutex);
  require(impl_->nextFence != UINT32_MAX, "menu backend fence budget exhausted");
  const uint32_t fence = ++impl_->nextFence;
  require(impl_->fences.size() < 4096, "menu backend pending fence budget exceeded");
  impl_->fences[fence] = impl_->submitted;
  return fence;
}
bool MenuBackend::FencePending(uint32_t fence) {
  std::lock_guard lock(impl_->mutex);
  require(fence && fence <= impl_->nextFence, "menu backend unknown fence");
  if (fence <= impl_->completedFence) { return false; }
  const auto found = impl_->fences.find(fence);
  require(found != impl_->fences.end(), "menu backend unknown fence");
  return found->second > impl_->completed;
}
void MenuBackend::WaitFence(uint32_t fence) { if (FencePending(fence)) { Flush(); } }
bool MenuBackend::Busy(uint32_t resource) {
  std::lock_guard lock(impl_->mutex);
  for (const auto& [location, item] : impl_->resources) {
    if (item.views.contains(resource)) { return item.command > impl_->completed; }
  }
  return impl_->byAddress(resource).command > impl_->completed;
}
void MenuBackend::WaitResource(uint32_t resource) { if (Busy(resource)) { Flush(); } }
void MenuBackend::WaitGpuFrame(uint64_t frame) {
  std::lock_guard lock(impl_->mutex);
  require(frame < impl_->frames, "menu backend cannot wait for unsubmitted frame");
  if (frame >= impl_->finishedFrames) { Flush(); }
}

}
