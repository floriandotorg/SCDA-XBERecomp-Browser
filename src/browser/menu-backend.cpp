#include "browser/menu-backend.h"
#include "browser/menu-shaders.h"
#include "browser/menu-texture.h"
#include "browser/render-snapshot.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace browser {
namespace {

std::atomic<uint32_t> resourceSequence{0};

const auto require = [](bool valid, const char* reason) {
  if (!valid) { throw std::runtime_error(reason); }
};
uint32_t address(uint32_t value) {
  if (value >= 0xf0000000 && value < 0xf4000000) { return (value & 0x03ffffff) | 0x80000000; }
  return value && value < 0x04000000 ? value | 0x80000000 : value;
}
template<class T> void append(std::vector<uint8_t>& bytes, const T& value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + sizeof(value));
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void appendBytes(std::vector<uint8_t>& bytes, std::span<const uint8_t> value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + value.size());
  if (!value.empty()) { std::memcpy(bytes.data() + offset, value.data(), value.size()); }
}
std::vector<uint8_t> shaderKey(const gpu::State& state, uint16_t regs, bool pretransformed,
    const gpu::TexInfo (&textures)[4], bool colorTarget, bool depthBias) {
  std::vector<uint8_t> key;
  key.reserve(1024 + (state.vs ? state.vs->program.size() * 4 : 0));
  append(key, regs); append(key, pretransformed); append(key, colorTarget); append(key, depthBias);
  append(key, state.vs_handle & 1); append(key, bool(state.vs));
  append(key, gpu::MakePixelKey(state, textures)); append(key, gpu::MakeFixedKey(state));
  if ((state.vs_handle & 1) && state.vs) {
    append(key, state.vs->program_type); append(key, uint32_t(state.vs->program.size()));
    appendBytes(key, {reinterpret_cast<const uint8_t*>(state.vs->program.data()), state.vs->program.size() * 4});
  }
  for (const auto index : {gpu::kRsFogEnable, gpu::kRsRangeFogEnable, gpu::kRsVertexBlend,
      gpu::kRsPointSpriteEnable, gpu::kRsPointScaleEnable, gpu::kRsWrap0}) { append(key, state.rs[index]); }
  const uint32_t alpha = state.simple[gpu::kMAlphaFunc / 4];
  append(key, state.simple[gpu::kMAlphaTestEnable / 4] && (alpha < 0x200 || alpha > 0x207) ? alpha : 0u);
  for (uint32_t n = 0; n < 4; ++n) {
    const auto& sampling = state.tss[n];
    append(key, sampling[gpu::kTssColorKeyOp]); append(key, sampling[gpu::kTssColorSign]);
    if (textures[n].shadow || (textures[n].kind == gpu::kTex2D &&
        (sampling[gpu::kTssAddressU] == 4 || sampling[gpu::kTssAddressV] == 4))) {
      for (const auto index : {gpu::kTssAddressU, gpu::kTssAddressV, gpu::kTssMagFilter, gpu::kTssMinFilter,
          gpu::kTssMipFilter, gpu::kTssMaxMipLevel, gpu::kTssBorderColor}) { append(key, sampling[index]); }
      if (textures[n].shadow) { append(key, state.rs[gpu::kRsShadowFunc]); }
    }
  }
  return key;
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
    case 3: case 4: return 1;
    default: throw std::runtime_error("menu backend unsupported sampler address mode");
  }
}
}

struct MenuBackend::Impl {
  struct Resource {
    uint32_t id = 0;
    uint64_t version = 1;
    uint32_t command = 0;
    uint32_t feedback = 0;
    uint64_t feedbackVersion = 0;
    gpu::TextureDesc desc;
    uint32_t headerAddress = 0;
    std::vector<uint8_t> header;
    std::set<uint32_t> views;
    std::vector<uint8_t> uploaded;
    std::vector<uint8_t> late;
    size_t residentBytes = 0;
    bool depth = false;
    bool renderTarget = false;
    bool depthInitialized = false;
    bool dirty = false;
    uint64_t lateVersion = 0;
    std::optional<uint64_t> cpuVersion;
  };
  Runtime& runtime;
  PreparedImage& image;
  Memory* memory;
  Execute execute;
  MenuBackendLimits limits;
  ShaderInventory inventory;
  std::function<void(const FrameMetrics&)> metricsSink;
  uint64_t commandWaitMicroseconds = 0, readbackWaitMicroseconds = 0, uploadWaitMicroseconds = 0, drawWaitMicroseconds = 0;
  mutable std::recursive_mutex mutex;
  std::map<uint64_t, Resource> resources;
  std::map<std::vector<uint8_t>, uint32_t> pipelines;
  std::map<std::vector<uint8_t>, menu::ShaderResult> shaderCache;
  uint64_t shaderTranslations = 0;
  size_t shaderCacheBytes = 0;
  std::map<uint32_t, std::pair<uint32_t, uint64_t>> uploadVersions;
  uint32_t white = 0;
  uint32_t width = 0, height = 0;
  uint32_t submitted = 0, completed = 0;
  uint64_t submittedCount = 0, completedCount = 0;
  uint32_t nextFence = 0, completedFence = 0;
  uint64_t frames = 0, finishedFrames = 0;
  size_t resident = 0;
  std::string storageViewFailure;
  std::vector<uint8_t> stagedDraws;
  std::set<uint32_t> stagedTargets;
  uint32_t stagedDrawCount = 0;
  std::map<uint32_t, uint32_t> fences;
  bool initialized = false;
  bool retiring = false;
  bool retired = false;

  Impl(Runtime& owner, PreparedImage& prepared, Execute exchange, MenuBackendLimits budget)
    : runtime(owner), image(prepared), memory(prepared.memory.get()), execute(std::move(exchange)), limits(budget) {
    require(memory && execute && (!image.boundRuntime || image.boundRuntime == &runtime), "menu backend requires its runtime and prepared image");
  }
  void requireOwner() const {
    require(memory == image.memory.get() && (!image.boundRuntime || image.boundRuntime == &runtime), "menu backend foreign prepared image or runtime");
    require(!Memory::HasBorrow(), "menu backend cannot wait with borrowed guest memory");
    require(!retired, "menu backend is retired");
    if (!retiring) { runtime.Checkpoint({}); }
  }
  const menu::ShaderResult& shaders(const gpu::State& state, uint16_t regs, bool pretransformed,
      const gpu::TexInfo (&textures)[4], bool colorTarget, bool depthBias) {
    auto key = shaderKey(state, regs, pretransformed, textures, colorTarget, depthBias);
    if (const auto found = shaderCache.find(key); found != shaderCache.end()) { return found->second; }
    ++shaderTranslations;
    auto translated = menu::GenerateShaders(state, regs, pretransformed, textures, 0, colorTarget, depthBias);
    require(translated.supported, translated.diagnostic.c_str());
    const auto bytesOf = [](const auto& key, const auto& shader) {
      return key.capacity() + shader.vertex.capacity() + shader.fragment.capacity() + shader.state.capacity() + shader.diagnostic.capacity();
    };
    const size_t bytes = bytesOf(key, translated);
    require(bytes <= (16u << 20), "menu backend shader translation budget exceeded");
    while (!shaderCache.empty() && (shaderCache.size() >= 256 || shaderCacheBytes + bytes > (16u << 20))) {
      const auto found = shaderCache.begin();
      shaderCacheBytes -= bytesOf(found->first, found->second);
      shaderCache.erase(found);
    }
    shaderCacheBytes += bytes;
    return shaderCache.emplace(std::move(key), std::move(translated)).first->second;
  }
  webgpu::Result send(webgpu::Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes = {}) {
    if (opcode != webgpu::Opcode::DrawBatch) { flushDraws(); }
    requireOwner();
    require(bytes.size() <= webgpu::MaxBytes, "menu backend command payload budget exceeded");
    const auto started = std::chrono::steady_clock::now();
    auto result = execute(opcode, resource, bytes);
    const uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    commandWaitMicroseconds += elapsed;
    if (opcode == webgpu::Opcode::Readback) { readbackWaitMicroseconds += elapsed; }
    if (opcode == webgpu::Opcode::CreateTexture || opcode == webgpu::Opcode::CreatePipeline || opcode == webgpu::Opcode::UploadDepth) { uploadWaitMicroseconds += elapsed; }
    if (opcode == webgpu::Opcode::Draw || opcode == webgpu::Opcode::DrawBatch) { drawWaitMicroseconds += elapsed; }
    if (result.status != webgpu::Status::Ok) {
      throw std::runtime_error("menu backend GPU service failure: " + std::string(result.bytes.begin(), result.bytes.end()));
    }
    require(result.resource == resource && result.command > submitted, "menu backend GPU service invalid completion identity");
    submitted = result.command;
    require(submittedCount != UINT64_MAX, "menu backend command counter budget exceeded");
    ++submittedCount;
    if (opcode == webgpu::Opcode::Flush || opcode == webgpu::Opcode::Readback || opcode == webgpu::Opcode::Present) {
      completed = submitted; completedCount = submittedCount; finishedFrames = frames;
      for (auto found = fences.begin(); found != fences.end();) {
        if (found->second > completed) { break; }
        completedFence = found->first; found = fences.erase(found);
      }
    }
    return result;
  }
  void flushDraws() {
    if (!stagedDrawCount) { return; }
    std::memcpy(stagedDraws.data(), &stagedDrawCount, 4);
    const auto result = send(webgpu::Opcode::DrawBatch, 0, stagedDraws);
    for (auto& [key, item] : resources) {
      if (stagedTargets.contains(item.id)) { item.command = result.command; }
    }
    stagedDraws.clear(); stagedTargets.clear(); stagedDrawCount = 0;
  }
  std::optional<uint32_t> draw(uint32_t resource, const webgpu::DrawPacket& packet, std::span<const uint8_t> bytes) {
    if (!limits.batchDraws) { return send(webgpu::Opcode::Draw, resource, bytes).command; }
    requireOwner();
    require(bytes.size() + 12 <= webgpu::MaxBytes, "menu backend staged draw exceeds packet budget");
    if (stagedDrawCount == 256 || stagedDraws.size() + bytes.size() + 8 > webgpu::MaxBytes) { flushDraws(); }
    if (!stagedDrawCount) { stagedDraws.reserve(webgpu::MaxBytes); stagedDraws.resize(4); }
    append(stagedDraws, resource); append(stagedDraws, uint32_t(bytes.size())); appendBytes(stagedDraws, bytes);
    if (resource) { stagedTargets.insert(resource); }
    if (packet.depth_target) { stagedTargets.insert(packet.depth_target); }
    ++stagedDrawCount;
    return std::nullopt;
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
    if (surface) { desc.cube = false; desc.depth = 1; desc.levels = 1; }
    if (desc.depth != 1 || (desc.cube && (desc.width != desc.height || desc.linear))) {
      throw std::runtime_error("menu backend unsupported texture dimension: format=" + std::to_string(format) + " width=" + std::to_string(desc.width) +
        " height=" + std::to_string(desc.height) + " depth=" + std::to_string(desc.depth) + " levels=" + std::to_string(desc.levels) + " cube=" + std::to_string(desc.cube));
    }
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
  size_t resourceCount() const {
    return resources.size() + size_t(white != 0) + std::count_if(resources.begin(), resources.end(), [](const auto& entry) { return entry.second.feedback != 0; });
  }
  void observeView(Resource& item, uint32_t header) {
    for (auto& [data, other] : resources) {
      if (&other != &item) { other.views.erase(header); }
    }
    item.views.insert(header);
  }
  Resource& ensure(uint32_t headerAddress, bool surface, bool depth = false) {
    require(headerAddress, "menu backend missing target resource");
    auto header = read(headerAddress, 20);
    auto desc = describe(header, surface);
    if (surface && !depth) {
      require(desc.format == 0x06 || desc.format == 0x07 || desc.format == 0x12 || desc.format == 0x1e, ("menu backend unsupported color surface format=" + std::to_string(desc.format)).c_str());
    }
    const uint64_t bytes = resourceBytes(desc, depth);
    require(bytes <= limits.maxResourceBytes, "menu backend resource byte budget exceeded");
    const uint64_t key = uint64_t(desc.data) * 2 + uint32_t(depth);
    auto found = resources.find(key);
    const auto incompatible = [&](const char* reason, const Resource& item) {
      std::ostringstream diagnostic;
      diagnostic << "menu backend unsupported " << reason << " GPU resource view";
      const auto view = [&](const char* label, uint32_t location, const auto& words, const auto& layout, bool isDepth, uint64_t count) {
        diagnostic << " " << label << "{header=0x" << std::hex << location << " common=0x" << word(words, 0)
          << " data=0x" << layout.data << " format=0x" << word(words, 12) << " size=0x" << word(words, 16)
          << std::dec << " width=" << layout.width << " height=" << layout.height << " pitch=" << layout.pitch
          << " levels=" << layout.levels << " cube=" << layout.cube << " linear=" << layout.linear << " depth=" << isDepth << " bytes=" << count;
      };
      view("requested", headerAddress, header, desc, depth, bytes); diagnostic << "}";
      view("existing", item.headerAddress, item.header, item.desc, item.depth, item.uploaded.size());
      diagnostic << " views=" << item.views.size() << " version=" << item.version << " dirty=" << item.dirty << "}";
      storageViewFailure = diagnostic.str();
      throw std::runtime_error(storageViewFailure);
    };
    bool replacedStorage = false;
    std::optional<uint64_t> cpuVersion;
    std::vector<uint8_t> changedSource;
    if (found != resources.end()) {
      auto& item = found->second;
      const bool compatible = item.depth == depth && item.desc.format == desc.format && item.desc.width == desc.width && item.desc.height == desc.height &&
        item.desc.pitch == desc.pitch && item.desc.levels == desc.levels && item.desc.cube == desc.cube && item.desc.linear == desc.linear && item.uploaded.size() == bytes;
      if (!compatible) {
        if (item.dirty || !item.late.empty() || item.views.contains(headerAddress)) { incompatible("incompatible storage", item); }
        replacedStorage = true;
      } else {
        require(item.views.contains(headerAddress) || item.views.size() < limits.maxResources, "menu backend resource view count budget exceeded");
        if (!item.views.contains(headerAddress)) { observeView(item, headerAddress); }
        item.headerAddress = headerAddress; item.header = header;
        cpuVersion = memory->PhysicalWriteVersion({}, desc.data, size_t(bytes));
        if (cpuVersion && item.cpuVersion == cpuVersion) { return item; }
        auto source = read(desc.data, bytes);
        if (source == item.uploaded) { item.cpuVersion = cpuVersion; return item; }
        if (depth && resources.contains(uint64_t(desc.data) * 2)) {
          item.uploaded = std::move(source); item.cpuVersion = cpuVersion;
          return item;
        }
        require(!depth, "menu backend unsupported CPU depth surface upload");
        changedSource = std::move(source);
      }
    }
    for (const auto& [location, item] : resources) {
      if (item.depth == depth && item.desc.data != desc.data && uint64_t(desc.data) + bytes > item.desc.data && uint64_t(item.desc.data) + item.uploaded.size() > desc.data) {
        incompatible("overlapping", item);
      }
    }
    if (!cpuVersion) { cpuVersion = memory->PhysicalWriteVersion({}, desc.data, size_t(bytes)); }
    auto source = changedSource.empty() ? read(desc.data, bytes) : std::move(changedSource);
    std::vector<uint8_t> rgba;
    if (!depth) {
      for (uint32_t n = 0; n < desc.levels * (desc.cube ? 6 : 1); ++n) {
        auto decoded = menu::DecodeTexture(desc, source, {}, n % desc.levels, n / desc.levels);
        require(decoded.supported, decoded.diagnostic.c_str());
        rgba.insert(rgba.end(), decoded.pixels.begin(), decoded.pixels.end());
      }
    }
    const size_t residentBytes = size_t(bytes) + (depth ? size_t(desc.width) * desc.height * 4 : rgba.size());
    const size_t oldBytes = found == resources.end() ? 0 : found->second.residentBytes + found->second.late.size();
    require(resourceCount() < limits.maxResources || found != resources.end(), "menu backend resource count budget exceeded");
    require(residentBytes <= limits.maxResidentBytes && resident - std::min(resident, oldBytes) <= limits.maxResidentBytes - residentBytes, ("menu backend resident byte budget exceeded: resident=" + std::to_string(resident) + " replacement=" + std::to_string(oldBytes) + " request=" + std::to_string(residentBytes) + " limit=" + std::to_string(limits.maxResidentBytes)).c_str());
    const uint32_t id = newId();
    webgpu::TexturePacket packet{desc.width, desc.height, depth ? 1u : desc.cube ? 2u : 0u, desc.levels};
    std::vector<uint8_t> payload;
    append(payload, packet); appendBytes(payload, rgba);
    const auto result = send(webgpu::Opcode::CreateTexture, id, payload);
    uint64_t version = 1;
    if (found != resources.end()) {
      version = found->second.version + 1;
      send(webgpu::Opcode::Destroy, found->second.id);
      if (found->second.feedback) { send(webgpu::Opcode::Destroy, found->second.feedback); }
      resident -= std::min(resident, oldBytes);
    }
    Resource item;
    item.id = id; item.version = version; item.command = result.command; item.cpuVersion = cpuVersion;
    item.desc = desc; item.headerAddress = headerAddress; item.header = std::move(header); item.uploaded = std::move(source); item.depth = depth; item.residentBytes = residentBytes;
    if (found != resources.end() && !replacedStorage) { item.views = std::move(found->second.views); }
    item.views.insert(headerAddress);
    resources[key] = std::move(item);
    resident += residentBytes;
    auto& stored = resources.at(key);
    observeView(stored, headerAddress);
    return stored;
  }
  Resource& surface(const gpu::Surface& value, bool depth = false) {
    auto& item = ensure(value.header, true, depth);
    item.renderTarget = true;
    require(item.desc.data == address(value.data) && word(item.header, 12) == value.format && word(item.header, 16) == value.size, "menu backend stale or incompatible surface descriptor");
    return item;
  }
  void initializeDepth(Resource& item) {
    if (item.depthInitialized) { return; }
    const auto format = item.desc.format;
    const bool d24 = format == 0x2a || format == 0x2e;
    const bool d16 = format == 0x2c || format == 0x30;
    require(d24 || d16, "menu backend unsupported uncleared depth format");
    const uint32_t stride = d24 ? 4 : 2;
    require(item.uploaded.size() >= stride && item.uploaded.size() % stride == 0,
      "menu backend unsupported uncleared depth storage");
    const auto bitsAt = [&](size_t offset) {
      uint32_t bits = 0;
      for (uint32_t n = 0; n < stride; ++n) { bits |= uint32_t(item.uploaded[offset + n]) << (n * 8); }
      return bits;
    };
    bool uniform = true;
    for (size_t n = stride; n < item.uploaded.size(); ++n) {
      if (item.uploaded[n] != item.uploaded[n % stride]) { uniform = false; break; }
    }
    webgpu::Result result;
    if (uniform) {
      const auto bits = bitsAt(0);
      webgpu::ClearPacket packet{};
      packet.flags = 6;
      packet.depth_target = item.id;
      packet.depth = d24 ? float(bits >> 8) / 16777215.0f : float(bits) / 65535.0f;
      packet.stencil = d24 ? bits & 255 : 0;
      result = sendPacket(webgpu::Opcode::Clear, 0, packet);
    } else {
      require(item.desc.linear, "menu backend unsupported nonuniform swizzled depth import");
      webgpu::DepthUploadPacket packet{item.desc.width, item.desc.height, d24 ? 24u : 16u, {}};
      const uint64_t count = uint64_t(packet.width) * packet.height;
      require(count * 4 + sizeof(packet) <= webgpu::MaxBytes, "menu backend depth import exceeds packet budget");
      std::vector<uint8_t> payload(sizeof(packet) + size_t(count) * 4);
      for (size_t n = 0; n < count; ++n) {
        const size_t offset = (n / packet.width) * item.desc.pitch + (n % packet.width) * stride;
        const uint32_t bits = d24 ? bitsAt(offset) : bitsAt(offset) << 8;
        packet.stencil_mask[(bits & 255) / 32] |= 1u << (bits & 31);
        std::memcpy(payload.data() + sizeof(packet) + n * 4, &bits, 4);
      }
      std::memcpy(payload.data(), &packet, sizeof(packet));
      result = send(webgpu::Opcode::UploadDepth, item.id, payload);
    }
    item.command = result.command;
    ++item.version;
    item.depthInitialized = true;
  }
  uint32_t feedbackTexture(Resource& item) {
    require(!item.depth, "menu backend unsupported depth feedback sampling");
    if (!item.feedback) {
      const size_t bytes = size_t(item.desc.width) * item.desc.height * 4;
      require(resourceCount() < limits.maxResources, "menu backend feedback resource count budget exceeded");
      require(bytes <= limits.maxResourceBytes && bytes <= limits.maxResidentBytes && resident <= limits.maxResidentBytes - bytes, "menu backend feedback resident byte budget exceeded");
      std::vector<uint8_t> payload;
      append(payload, webgpu::TexturePacket{item.desc.width, item.desc.height, 0});
      payload.resize(payload.size() + bytes, 0);
      const uint32_t id = newId();
      send(webgpu::Opcode::CreateTexture, id, payload);
      item.feedback = id; item.residentBytes += bytes; resident += bytes;
    }
    if (item.feedbackVersion != item.version) {
      sendPacket(webgpu::Opcode::Copy, item.feedback, webgpu::CopyPacket{item.id, item.desc.width, item.desc.height});
      item.feedbackVersion = item.version;
    }
    return item.feedback;
  }
  uint32_t whiteTexture() {
    if (!white) {
      require(resourceCount() < limits.maxResources, "menu backend resource count budget exceeded");
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
    for (uint32_t n = 0; n < 2; ++n) {
      const auto found = resources.find(uint64_t(location) * 2 + n);
      if (found != resources.end()) { return found->second; }
    }
    Resource* selected = nullptr;
    for (auto& [key, item] : resources) {
      if (location >= item.desc.data && uint64_t(location) < uint64_t(item.desc.data) + item.uploaded.size()) {
        require(!selected, "menu backend ambiguous overlapping GPU resource address");
        selected = &item;
      }
    }
    require(selected, "menu backend readback unknown resource");
    return *selected;
  }
  bool readHeader(const Resource& item) {
    for (uint32_t location : item.views) {
      auto header = read(location, 20);
      if (address(word(header, 4)) == item.desc.data && word(header, 12) == word(item.header, 12) && word(header, 16) == word(item.header, 16)) { return true; }
    }
    return false;
  }
  std::vector<uint8_t> readback(Resource& item) {
    require(!item.depth, "menu backend unsupported depth readback resource format");
    const auto result = send(webgpu::Opcode::Readback, item.id);
    auto encoded = menu::EncodeSurface(item.desc, item.uploaded, result.bytes);
    require(encoded.supported, encoded.diagnostic.c_str());
    return std::move(encoded.bytes);
  }

  void publish(Resource& item, std::span<const uint8_t> bytes, uint32_t offset, uint32_t size) {
    memory->CheckRange({}, item.desc.data + offset, size);
    const uint32_t rows = item.desc.linear ? item.desc.height : 1;
    const size_t pitch = item.desc.linear ? item.desc.pitch : 0;
    const size_t rowBytes = item.desc.linear ? size_t(item.desc.width) * 4 : item.uploaded.size();
    for (uint32_t n = 0; n < rows; ++n) {
      const size_t first = std::max(size_t(offset), size_t(n) * pitch);
      const size_t last = std::min(size_t(offset) + size, size_t(n) * pitch + rowBytes);
      if (first < last) {
        memory->Write({}, item.desc.data + uint32_t(first), bytes.data() + first, last - first);
        std::copy_n(bytes.data() + first, last - first, item.uploaded.data() + first);
        for (auto& [key, other] : resources) {
          if (&other == &item) { continue; }
          const uint64_t begin = std::max(uint64_t(item.desc.data) + first, uint64_t(other.desc.data));
          const uint64_t end = std::min(uint64_t(item.desc.data) + last, uint64_t(other.desc.data) + other.uploaded.size());
          if (begin < end) { std::copy_n(bytes.data() + (begin - item.desc.data), end - begin, other.uploaded.data() + (begin - other.desc.data)); }
        }
      }
    }
    if (offset == 0 && size == item.uploaded.size()) { item.uploaded = read(item.desc.data, item.uploaded.size()); }
  }
  void checkState(const gpu::State& state) {
    require(!state.rs[gpu::kRsLogicOp], "menu backend unsupported logic operation");
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
  try { Retire(); } catch (...) {}
}
void MenuBackend::SetMetricsSink(std::function<void(const FrameMetrics&)> sink) {
  std::lock_guard lock(impl_->mutex);
  require(!impl_->runtime.ActiveWorkers(), "menu backend metrics sink requires idle workers");
  impl_->metricsSink = std::move(sink);
}
void MenuBackend::Retire() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->retired) { return; }
  require(!impl_->runtime.ActiveWorkers() && !Memory::HasBorrow(), "menu backend retirement requires joined guest workers");
  impl_->stagedDraws.clear(); impl_->stagedTargets.clear(); impl_->stagedDrawCount = 0;
  impl_->retiring = true;
  struct RetirementScope {
    bool& active;
    ~RetirementScope() { active = false; }
  } scope{impl_->retiring};
  while (!impl_->pipelines.empty()) {
    const auto found = impl_->pipelines.begin();
    impl_->send(webgpu::Opcode::Destroy, found->second);
    impl_->pipelines.erase(found);
  }
  while (!impl_->resources.empty()) {
    const auto found = impl_->resources.begin();
    auto& item = found->second;
    if (item.feedback) {
      impl_->send(webgpu::Opcode::Destroy, item.feedback);
      item.feedback = 0;
    }
    impl_->send(webgpu::Opcode::Destroy, item.id);
    impl_->resources.erase(found);
  }
  if (impl_->white) {
    impl_->send(webgpu::Opcode::Destroy, impl_->white);
    impl_->white = 0;
  }
  impl_->resident = 0;
  impl_->shaderCache.clear(); impl_->shaderCacheBytes = 0;
  impl_->initialized = false;
  impl_->retired = true;
}
MenuBackend::SnapshotData MenuBackend::Snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return {uint32_t(impl_->pipelines.size()), uint32_t(impl_->resourceCount()), impl_->resident,
    impl_->submittedCount, impl_->completedCount, impl_->frames, impl_->storageViewFailure,
    impl_->shaderTranslations, uint32_t(impl_->shaderCacheBytes), uint32_t(impl_->shaderCache.size())};
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
  require(order.size() <= (impl_->limits.maxExpandedVertices ? impl_->limits.maxExpandedVertices : impl_->limits.maxVertices), "menu backend expanded vertex budget exceeded");
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
  uint32_t maximum = 0, minimum = UINT32_MAX;
  for (uint32_t n = 0; n < count; ++n) {
    const uint64_t vertex = indices ? uint64_t(indexValues[n]) + input.base_vertex : uint64_t(start) + n;
    require(vertex <= UINT32_MAX, "menu backend vertex address overflow");
    maximum = std::max(maximum, uint32_t(vertex)); minimum = std::min(minimum, uint32_t(vertex)); vertexNumbers[n] = uint32_t(vertex);
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
    if (kind == RenderResourceKind::Texture || kind == RenderResourceKind::Surface) {
      kind = kind == RenderResourceKind::Texture ? RenderResourceKind::TextureReference : RenderResourceKind::SurfaceReference;
      bytes = 0;
    }
    if (header && requestIndex.contains(header)) {
      auto& request = requests.at(requestIndex.at(header));
      require(request.kind == kind && request.payloadAddress == data, "menu backend incompatible shared resource binding");
      request.payloadBytes = std::max(request.payloadBytes, bytes);
      return;
    }
    if (header) { requestIndex[header] = requests.size(); }
    uint32_t id = 0;
    if (kind == RenderResourceKind::TextureReference || kind == RenderResourceKind::SurfaceReference) {
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
  std::array<uint32_t, 4> textureFormats{};
  std::array<std::array<float, 4>, 4> texsize{};
  webgpu::DrawPacket packet{};
  for (uint32_t n = 0; n < 4; ++n) {
    if (input.textures[n]) {
      const auto header = impl_->read(input.textures[n], 20);
      const uint32_t format = (word(header, 12) >> 8) & 255;
      const bool depth = format == 0x2a || format == 0x2c || format == 0x2e || format == 0x30;
      auto& item = impl_->ensure(input.textures[n], false, depth);
      if (depth) {
        require(!input.zs.data || item.desc.data != address(input.zs.data) || (!input.rs[gpu::kRsZEnable] && !input.rs[gpu::kRsStencilEnable]), "menu backend sampled active depth attachment");
        impl_->initializeDepth(item);
        texinfo[n].shadow = format == 0x2c || format == 0x30 ? 2 : 1;
      }
      textureFormats[n] = item.desc.format;
      texinfo[n].kind = item.desc.cube ? gpu::kTexCube : gpu::kTex2D; texinfo[n].rect = item.desc.linear;
      texsize[n] = {float(item.desc.width), float(item.desc.height), item.renderTarget ? -0.5f / item.desc.width : 0, item.renderTarget ? -0.5f / item.desc.height : 0};
      packet.texture[n] = input.rt.data && item.desc.data == address(input.rt.data) ? impl_->feedbackTexture(item) : item.id;
      addRequest(RenderResourceKind::Texture, input.textures[n], item.desc.data, uint32_t(item.uploaded.size()), item.version, item.id);
      const auto& sampling = input.tss[n];
      packet.sampler[n][0] = samplerAddress(sampling[gpu::kTssAddressU]);
      packet.sampler[n][1] = samplerAddress(sampling[gpu::kTssAddressV]);
      const uint32_t mag = sampling[gpu::kTssMagFilter], min = sampling[gpu::kTssMinFilter], mip = sampling[gpu::kTssMipFilter];
      require(mag >= 1 && mag <= 2 && min >= 1 && min <= 2 && mip <= 2, "menu backend unsupported sampler filter");
      const float bias = std::bit_cast<float>(sampling[gpu::kTssMipLodBias]);
      require(std::isfinite(bias), "menu backend unsupported nonfinite sampler LOD bias");
      require((item.desc.levels == 1 && min == mag) || (bias >= -16 && bias < 16), "menu backend sampler LOD bias exceeds WebGPU range");
      packet.sampler[n][2] = mag; packet.sampler[n][3] = min; packet.sampler[n][4] = mip == 2 ? 2 : 1;
      packet.sampler[n][5] = std::min(sampling[gpu::kTssMaxMipLevel], item.desc.levels - 1);
      packet.sampler[n][6] = mip ? item.desc.levels - 1 : packet.sampler[n][5];
    } else {
      packet.texture[n] = impl_->whiteTexture();
      std::fill_n(packet.sampler[n], 5, 1);
    }
  }
  const bool depthOnly = !input.rt.data && !input.simple[gpu::kMColorWriteEnable / 4] && input.zs.header &&
    (input.rs[gpu::kRsZEnable] || input.rs[gpu::kRsStencilEnable]);
  auto* colorTarget = depthOnly ? nullptr : &impl_->surface(input.rt);
  if (colorTarget) { addRequest(RenderResourceKind::Surface, input.rt.header, colorTarget->desc.data, uint32_t(colorTarget->uploaded.size()), colorTarget->version, colorTarget->id); }
  Impl::Resource* depthTarget = nullptr;
  if (input.zs.header && (input.rs[gpu::kRsZEnable] || input.rs[gpu::kRsStencilEnable])) {
    depthTarget = &impl_->surface(input.zs, true);
    require(!colorTarget || (depthTarget->desc.width == colorTarget->desc.width && depthTarget->desc.height == colorTarget->desc.height), "menu backend incompatible depth target dimensions");
    impl_->initializeDepth(*depthTarget);
    packet.depth_target = depthTarget->id;
    addRequest(RenderResourceKind::Surface, input.zs.header, depthTarget->desc.data, uint32_t(depthTarget->uploaded.size()), depthTarget->version, depthTarget->id);
  }
  require(colorTarget || depthTarget, "menu backend draw requires a color or depth target");
  auto& target = colorTarget ? *colorTarget : *depthTarget;
  if (input.index_buffer) {
    auto header = impl_->read(input.index_buffer, 12);
    const uint32_t data = address(word(header, 4));
    require(!input.index_data || address(input.index_data) == data, "menu backend incompatible index buffer data binding");
    addRequest(RenderResourceKind::IndexBuffer, input.index_buffer, data, indices ? count * 2 : 1, 1);
  } else if (input.index_data) {
    addRequest(RenderResourceKind::IndexData, 0, address(input.index_data), indices ? count * 2 : 1, 1);
  }
  RenderSnapshotLimits snapshotLimits;
  snapshotLimits.maxResources = budget.maxResources; snapshotLimits.maxPayloadBytes = budget.maxResourceBytes;
  snapshotLimits.maxTotalBytes = budget.maxResidentBytes;
  auto consumedState = input;
  if (!packet.depth_target) { consumedState.zs = {}; }
  if (!colorTarget) { consumedState.rt = {}; }
  auto snapshot = CaptureRenderSnapshot({}, *impl_->memory, consumedState, requests, snapshotLimits);
  auto& state = snapshot.state;
  if (snapshot.vertexShader) { state.vs = std::make_shared<gpu::VertexShader>(*snapshot.vertexShader); }
  if (snapshot.pixelShader) { state.ps = std::make_shared<gpu::PixelShader>(*snapshot.pixelShader); }
  const bool pretransformed = !(state.vs_handle & 1) && (state.vs_handle & 14) == 4;
  const bool depthBias = packet.depth_target && prim >= 5 && state.simple[gpu::kMSolidOffsetEnable / 4];
  const auto& shaders = impl_->shaders(state, regs, pretransformed, texinfo, colorTarget != nullptr, depthBias);
  impl_->inventory.Observe(state, pretransformed, texinfo);
  for (uint32_t n = 0; n < 4; ++n) {
    if (state.textures[n]) { impl_->inventory.Texture(textureFormats[n]); }
  }
  webgpu::PipelinePacket pipeline{};
  pipeline.vertex_mask = budget.compactVertices ? regs : 0;
  pipeline.color_target = colorTarget != nullptr;
  for (uint32_t n = 0; n < 4; ++n) { if (texinfo[n].kind == gpu::kTexCube) { pipeline.cube_mask |= 1u << n; } if (texinfo[n].shadow) { pipeline.shadow_mask |= 1u << n; } }
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
  if (colorTarget && (colorTarget->desc.format == 0x07 || colorTarget->desc.format == 0x1e)) { pipeline.write_mask &= 7; }
  pipeline.depth = packet.depth_target != 0;
  pipeline.depth_compare = 8;
  if (pipeline.depth && state.rs[gpu::kRsZEnable]) {
    const uint32_t compare = state.simple[gpu::kMZFunc / 4];
    require(compare >= 0x200 && compare <= 0x207, "menu backend unsupported depth comparison");
    pipeline.depth_compare = compare - 0x200 + 1;
    pipeline.depth_write = state.simple[gpu::kMZWriteEnable / 4] != 0;
  }
  if (pipeline.depth && state.rs[gpu::kRsStencilEnable]) {
    const auto operation = [](uint32_t value) -> uint32_t {
      switch (value) {
        case 0: return 2;
        case 0x1e00: return 1;
        case 0x1e01: return 3;
        case 0x1e02: return 5;
        case 0x1e03: return 6;
        case 0x150a: return 4;
        case 0x8507: return 7;
        case 0x8508: return 8;
        default: throw std::runtime_error("menu backend unsupported stencil operation");
      }
    };
    const uint32_t compare = state.simple[gpu::kMStencilFunc / 4];
    require(compare >= 0x200 && compare <= 0x207, "menu backend unsupported stencil comparison");
    pipeline.stencil = 1;
    pipeline.stencil_compare = compare - 0x200 + 1;
    pipeline.stencil_fail = operation(state.rs[gpu::kRsStencilFail]);
    pipeline.stencil_depth_fail = operation(state.simple[gpu::kMStencilZFail / 4]);
    pipeline.stencil_pass = operation(state.simple[gpu::kMStencilPass / 4]);
    pipeline.stencil_read_mask = state.simple[gpu::kMStencilMask / 4] & 255;
    pipeline.stencil_write_mask = state.simple[gpu::kMStencilWriteMask / 4] & 255;
    packet.stencil_reference = state.simple[gpu::kMStencilRef / 4] & 255;
  }
  std::vector<uint8_t> key;
  append(key, pipeline); append(key, regs); append(key, prim);
  for (const auto& element : layout.inputs) { append(key, element.stream); append(key, element.offset); append(key, element.type); }
  for (const auto& stream : state.streams) { append(key, stream.stride); }
  append(key, layout.stream_stride); append(key, state.vs_handle);
  append(key, state.rt.format); append(key, state.rt.size); append(key, packet.depth_target ? state.zs.format : 0u);
  key.insert(key.end(), shaders.vertex.begin(), shaders.vertex.end()); key.insert(key.end(), shaders.fragment.begin(), shaders.fragment.end());
  auto foundPipeline = impl_->pipelines.find(key);
  if (foundPipeline == impl_->pipelines.end()) {
    require(impl_->pipelines.size() < budget.maxPipelines, "menu backend pipeline count budget exceeded");
    std::vector<uint8_t> bytes;
    append(bytes, pipeline); bytes.insert(bytes.end(), shaders.vertex.begin(), shaders.vertex.end()); bytes.insert(bytes.end(), shaders.fragment.begin(), shaders.fragment.end());
    const uint32_t id = impl_->newId();
    try { impl_->send(webgpu::Opcode::CreatePipeline, id, bytes); }
    catch (const std::exception& error) { throw std::runtime_error(menu::DescribeShaderFailure(shaders, error.what())); }
    foundPipeline = impl_->pipelines.emplace(std::move(key), id).first;
  }
  packet.pipeline = foundPipeline->second;
  auto constants = menu::BuildConstants(state, target.desc.width, target.desc.height, texsize);
  if (depthBias) {
    const float offset = std::bit_cast<float>(state.simple[gpu::kMDepthOffset / 4]), slope = std::bit_cast<float>(state.simple[gpu::kMSlopeScale / 4]);
    require(std::isfinite(offset) && std::isfinite(slope) && std::isfinite(state.zscale) && state.zscale > 0, "menu backend invalid depth bias state");
    const double units = std::round(double(offset) * 16777215.0 / state.zscale);
    require(units >= INT32_MIN && units <= INT32_MAX, "menu backend depth bias exceeds signed D24 units");
    constants.depth_bias = {float(units / 16777215.0), slope, 0, 0};
  }
  for (uint32_t n = 0; n < 4; ++n) {
    if (!state.textures[n] || (packet.sampler[n][6] == 0 && packet.sampler[n][2] == packet.sampler[n][3])) { constants.lod_bias[n] = 0; }
  }
  packet.uniform_bytes = sizeof(constants);
  require(uint64_t(state.viewport.x) + state.viewport.w <= target.desc.width && uint64_t(state.viewport.y) + state.viewport.h <= target.desc.height && state.viewport.w && state.viewport.h, "menu backend unsupported viewport outside target");
  require(std::isfinite(state.viewport.min_z) && std::isfinite(state.viewport.max_z) && state.viewport.min_z >= 0 && state.viewport.max_z <= 1 && state.viewport.min_z <= state.viewport.max_z, "menu backend unsupported viewport depth range");
  packet.viewport[2] = float(target.desc.width); packet.viewport[3] = float(target.desc.height);
  packet.viewport[5] = 1;
  uint32_t left = state.viewport.x, top = state.viewport.y;
  uint32_t right = left + state.viewport.w, bottom = top + state.viewport.h;
  if (state.scissor_count) {
    require(state.scissors[0] <= state.scissors[2] && state.scissors[1] <= state.scissors[3] && state.scissors[2] <= target.desc.width && state.scissors[3] <= target.desc.height, "menu backend unsupported scissor outside target");
    left = std::max(left, state.scissors[0]); top = std::max(top, state.scissors[1]);
    right = std::min(right, state.scissors[2]); bottom = std::min(bottom, state.scissors[3]);
  }
  packet.scissor[0] = left; packet.scissor[1] = top;
  packet.scissor[2] = std::max(right, left) - left; packet.scissor[3] = std::max(bottom, top) - top;
  const uint32_t primitiveVertices = pipeline.topology == 1 ? 1 : pipeline.topology == 2 ? 2 : 3;
  const uint32_t vertexRegisters = budget.compactVertices ? std::popcount(regs) : 16;
  const uint32_t vertexStride = vertexRegisters * 16;
  struct VertexSource { const uint8_t* first = nullptr; uint32_t stride = 0, type = 0; };
  std::array<VertexSource, 16> sources;
  uint32_t sourceCount = 0;
  for (uint32_t n = 0; n < 16; ++n) {
    if (budget.compactVertices && !(regs & (1u << n))) { continue; }
    auto& source = sources[sourceCount++];
    if (!(regs & (1u << n))) { continue; }
    const auto& element = layout.inputs[n];
    const auto& stream = state.streams[size_t(element.stream)];
    const auto& resource = snapshot.resources.at(requestIndex.at(stream.vb));
    source.stride = stream.stride ? stream.stride : layout.stream_stride[size_t(element.stream)];
    require(uint64_t(maximum) * source.stride + element.offset + typeBytes(element.type) <= resource.payload.size(), "menu backend vertex exceeds owned snapshot");
    source.first = resource.payload.data() + element.offset;
    source.type = element.type;
  }
  const uint32_t batchVertices = std::min(8192u, uint32_t((webgpu::MaxBytes - sizeof(packet) - sizeof(constants)) / (vertexStride + (budget.indexedVertices ? 2 : 0)))) / primitiveVertices * primitiveVertices;
  require(batchVertices, "menu backend uniforms leave no geometry staging capacity");
  std::vector<float> vertices;
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> batchNumbers;
  std::vector<uint16_t> batchIndices;
  std::vector<int32_t> remap(budget.indexedVertices ? size_t(maximum - minimum) + 1 : 0, -1);
  batchNumbers.reserve(batchVertices);
  if (budget.indexedVertices) { batchIndices.reserve(batchVertices); }
  for (size_t n = 0; n < order.size(); n += batchVertices) {
    const size_t batchStart = n;
    const uint32_t batchCount = uint32_t(std::min<size_t>(batchVertices, order.size() - batchStart));
    batchNumbers.clear(); batchIndices.clear();
    if (budget.indexedVertices) { std::fill(remap.begin(), remap.end(), -1); }
    for (uint32_t n = 0; n < batchCount; ++n) {
      const uint32_t number = vertexNumbers[order[batchStart + n]];
      if (budget.indexedVertices) {
        auto& index = remap[number - minimum];
        if (index < 0) { index = int32_t(batchNumbers.size()); batchNumbers.push_back(number); }
        batchIndices.push_back(uint16_t(index));
      } else { batchNumbers.push_back(number); }
    }
    vertices.assign(batchNumbers.size() * vertexRegisters * 4, 0);
    for (uint32_t n = 0; n < batchNumbers.size(); ++n) {
      const uint32_t vertex = n;
      const uint32_t number = batchNumbers[vertex];
      for (uint32_t n = 0; n < sourceCount; ++n) {
        auto* output = vertices.data() + size_t(vertex) * vertexRegisters * 4 + n * 4;
        output[3] = 1;
        const auto& source = sources[n];
        if (source.type) { convert(source.type, source.first + size_t(number) * source.stride, output); }
      }
    }
    packet.vertex_count = uint32_t(batchNumbers.size()); packet.vertex_bytes = uint32_t(vertices.size() * 4);
    packet.index_count = uint32_t(batchIndices.size()); packet.index_bytes = uint32_t(batchIndices.size() * 2);
    bytes.clear();
    bytes.reserve(sizeof(packet) + packet.vertex_bytes + packet.index_bytes + sizeof(constants));
    append(bytes, packet);
    appendBytes(bytes, {reinterpret_cast<const uint8_t*>(vertices.data()), vertices.size() * sizeof(float)});
    appendBytes(bytes, {reinterpret_cast<const uint8_t*>(batchIndices.data()), packet.index_bytes});
    append(bytes, constants);
    const auto command = impl_->draw(colorTarget ? colorTarget->id : 0, packet, bytes);
    if (colorTarget) { colorTarget->dirty = true; if (command) { colorTarget->command = *command; } ++colorTarget->version; }
    if (packet.depth_target) {
      auto& depth = impl_->resources.at(uint64_t(address(state.zs.data)) * 2 + 1);
      depth.dirty = true; if (command) { depth.command = *command; } ++depth.version;
    }
  }
}
void MenuBackend::Clear(const gpu::State& state, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
  std::lock_guard lock(impl_->mutex);
  require(impl_->initialized, "menu backend clear before initialization");
  require(count <= 4096 && (!count || rects), "menu backend invalid clear rectangle count or pointer");
  require((flags & ~(gpu::kClearTarget | gpu::kClearZ | gpu::kClearStencil)) == 0 && (!(flags & gpu::kClearTarget) || (flags & gpu::kClearTarget) == gpu::kClearTarget), "menu backend unsupported clear flags or color channels");
  if (!flags) { return; }
  require(std::isfinite(z) && z >= 0 && z <= 1, "menu backend unsupported clear depth");
  auto* target = flags & gpu::kClearTarget ? &impl_->surface(state.rt) : nullptr;
  auto* depth = flags & (gpu::kClearZ | gpu::kClearStencil) ? &impl_->surface(state.zs, true) : nullptr;
  webgpu::ClearPacket packet{};
  packet.color[0] = ((color >> 16) & 255) / 255.0f; packet.color[1] = ((color >> 8) & 255) / 255.0f;
  packet.color[2] = (color & 255) / 255.0f; packet.color[3] = (color >> 24) / 255.0f;
  packet.depth = z; packet.stencil = stencil & 255;
  if (target) { packet.flags |= 1; }
  if (flags & gpu::kClearZ) { packet.flags |= 2; }
  if (flags & gpu::kClearStencil) { packet.flags |= 4; }
  std::vector<uint8_t> rectangles;
  if (count) {
    packet.rect_count = count;
    rectangles.assign(rects, rects + size_t(count) * 16);
  } else {
    const auto& view = state.viewport;
    require(uint64_t(view.x) + view.w <= INT32_MAX && uint64_t(view.y) + view.h <= INT32_MAX, "menu backend clear viewport range overflow");
    if (view.x || view.y || (target && (view.w < target->desc.width || view.h < target->desc.height)) ||
        (depth && (view.w < depth->desc.width || view.h < depth->desc.height))) {
      const std::array<int32_t, 4> rectangle{int32_t(view.x), int32_t(view.y), int32_t(view.x + view.w), int32_t(view.y + view.h)};
      packet.rect_count = 1;
      appendBytes(rectangles, {reinterpret_cast<const uint8_t*>(rectangle.data()), sizeof(rectangle)});
    }
  }
  if (depth && (packet.rect_count || (packet.flags & 6) != 6)) { impl_->initializeDepth(*depth); }
  const auto submit = [&](Impl::Resource* colorTarget, Impl::Resource* depthTarget, uint32_t clearFlags) {
    auto request = packet;
    if (colorTarget && (colorTarget->desc.format == 0x07 || colorTarget->desc.format == 0x1e)) { request.color[3] = 1; }
    request.flags = clearFlags;
    request.depth_target = depthTarget ? depthTarget->id : 0;
    std::vector<uint8_t> bytes;
    append(bytes, request); appendBytes(bytes, rectangles);
    const auto result = impl_->send(webgpu::Opcode::Clear, colorTarget ? colorTarget->id : 0, bytes);
    for (auto* item : {colorTarget, depthTarget}) {
      if (!item) { continue; }
      item->depthInitialized |= item == depthTarget && (clearFlags & 2) != 0;
      item->dirty = true; item->command = result.command; ++item->version;
    }
  };
  if (target && depth && (target->desc.width != depth->desc.width || target->desc.height != depth->desc.height)) {
    submit(target, nullptr, 1); submit(nullptr, depth, packet.flags & 6);
  } else { submit(target, depth, packet.flags); }
}
void MenuBackend::Copy(const gpu::Surface& source, const gpu::Surface& destination, uint32_t count, uint32_t sourceRects, uint32_t destinationPoints) {
  std::lock_guard lock(impl_->mutex);
  require(impl_->initialized, "menu backend copy before initialization");
  require(count <= 4096 && (!count || sourceRects) && (count || (!sourceRects && !destinationPoints)), "menu backend invalid partial copy list");
  const auto rectangles = count ? impl_->read(sourceRects, uint64_t(count) * 16) : std::vector<uint8_t>{};
  const auto points = count && destinationPoints ? impl_->read(destinationPoints, uint64_t(count) * 8) : std::vector<uint8_t>{};
  const auto sourceHeader = impl_->read(source.header, 20), destinationHeader = impl_->read(destination.header, 20);
  const auto sourceDesc = impl_->describe(sourceHeader, true), destinationDesc = impl_->describe(destinationHeader, true);
  require(sourceDesc.data == address(source.data) && word(sourceHeader, 12) == source.format && word(sourceHeader, 16) == source.size &&
    destinationDesc.data == address(destination.data) && word(destinationHeader, 12) == destination.format && word(destinationHeader, 16) == destination.size,
    "menu backend stale copy surface descriptor");
  const auto regions = [&](uint32_t unit) {
    std::vector<webgpu::CopyPacket> result;
    const uint32_t sourceWidth = (sourceDesc.width + unit - 1) / unit, sourceHeight = (sourceDesc.height + unit - 1) / unit;
    const uint32_t destinationWidth = (destinationDesc.width + unit - 1) / unit, destinationHeight = (destinationDesc.height + unit - 1) / unit;
    for (uint32_t n = 0; n < std::max(count, 1u); ++n) {
      int64_t x0 = 0, y0 = 0, x1 = sourceWidth, y1 = sourceHeight, dx = 0, dy = 0;
      if (count) {
        x0 = int32_t(word(rectangles, n * 16)) / int64_t(unit);
        y0 = int32_t(word(rectangles, n * 16 + 4)) / int64_t(unit);
        x1 = (int64_t(int32_t(word(rectangles, n * 16 + 8))) + unit - 1) / unit;
        y1 = (int64_t(int32_t(word(rectangles, n * 16 + 12))) + unit - 1) / unit;
        dx = x0; dy = y0;
        if (!points.empty()) { dx = int32_t(word(points, n * 8)) / int64_t(unit); dy = int32_t(word(points, n * 8 + 4)) / int64_t(unit); }
      }
      const int64_t left = std::max({int64_t(0), x0, x0 - dx}), top = std::max({int64_t(0), y0, y0 - dy});
      const int64_t right = std::min({int64_t(sourceWidth), x1, x0 + destinationWidth - dx});
      const int64_t bottom = std::min({int64_t(sourceHeight), y1, y0 + destinationHeight - dy});
      if (right <= left || bottom <= top) { continue; }
      result.push_back({0, uint32_t(right - left), uint32_t(bottom - top), uint32_t(left), uint32_t(top), uint32_t(dx + left - x0), uint32_t(dy + top - y0)});
    }
    return result;
  };
  if (sourceDesc.format == 0x0c || sourceDesc.format == 0x0e || sourceDesc.format == 0x0f) {
    require(sourceDesc.format == destinationDesc.format, "menu backend incompatible compressed surface copy");
    const auto bytes = impl_->read(sourceDesc.data, impl_->resourceBytes(sourceDesc, false));
    auto destinationBytes = impl_->read(destinationDesc.data, impl_->resourceBytes(destinationDesc, false));
    const uint32_t blockBytes = sourceDesc.format == 0x0c ? 8 : 16;
    const uint32_t sourcePitch = ((sourceDesc.width + 3) / 4) * blockBytes, destinationPitch = ((destinationDesc.width + 3) / 4) * blockBytes;
    for (const auto& region : regions(4)) {
      for (uint32_t n = 0; n < region.height; ++n) {
        std::memcpy(destinationBytes.data() + (region.destination_y + n) * destinationPitch + region.destination_x * blockBytes,
          bytes.data() + (region.source_y + n) * sourcePitch + region.source_x * blockBytes, region.width * blockBytes);
      }
    }
    impl_->runtime.Checkpoint({});
    impl_->memory->Write({}, destinationDesc.data, destinationBytes.data(), destinationBytes.size());
    return;
  }
  auto& from = impl_->surface(source); auto& to = impl_->surface(destination);
  const auto colorFamily = [](uint32_t format) { return format == 0x06 || format == 0x12 ? 1u : format == 0x07 || format == 0x1e ? 2u : 0u; };
  require(colorFamily(from.desc.format) && colorFamily(to.desc.format), "menu backend incompatible surface copy");
  const auto copies = regions(1);
  if (copies.empty()) { return; }
  const uint32_t sourceId = from.id == to.id ? impl_->feedbackTexture(from) : from.id;
  for (auto packet : copies) {
    packet.source = sourceId;
    packet.force_opaque = colorFamily(to.desc.format) == 2;
    const auto result = impl_->sendPacket(webgpu::Opcode::Copy, to.id, packet);
    to.dirty = true; to.command = result.command; ++to.version;
  }
}
bool MenuBackend::HasGpuContents(uint32_t location) const {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  const auto found = impl_->resources.find(uint64_t(address(location)) * 2);
  return found != impl_->resources.end() && found->second.renderTarget && found->second.dirty;
}
void MenuBackend::Present(const gpu::Surface& front, bool pillarbox) {
  std::lock_guard lock(impl_->mutex);
  require(impl_->initialized, "menu backend present before initialization");
  auto& item = impl_->surface(front);
  const auto result = impl_->sendPacket(webgpu::Opcode::Present, item.id, webgpu::PresentPacket{uint32_t(pillarbox)});
  item.command = result.command; ++impl_->frames; impl_->finishedFrames = impl_->frames;
  if (impl_->metricsSink) {
    impl_->metricsSink({impl_->frames, impl_->commandWaitMicroseconds, impl_->readbackWaitMicroseconds, impl_->uploadWaitMicroseconds,
      impl_->drawWaitMicroseconds, uint32_t(impl_->resourceCount()), uint32_t(impl_->pipelines.size()), uint32_t(impl_->resident), impl_->inventory});
  }
}
void MenuBackend::Sync(uint32_t location, uint32_t size) {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  auto& item = impl_->byAddress(location);
  if (!size) { size = uint32_t(item.uploaded.size()) - (address(location) - item.desc.data); }
  require(uint64_t(address(location)) + size <= uint64_t(item.desc.data) + item.uploaded.size(), "menu backend readback range exceeds resource");
  auto bytes = impl_->readback(item);
  const size_t offset = address(location) - item.desc.data;
  impl_->publish(item, bytes, uint32_t(offset), size);
  if (offset == 0 && size == item.uploaded.size()) { item.dirty = false; }
}
void MenuBackend::RecordLate(uint32_t location) {
  std::lock_guard lock(impl_->mutex);
  auto& item = impl_->byAddress(location);
  const size_t extra = item.uploaded.size();
  require(impl_->readHeader(item), "menu backend stale resource header before late readback");
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
  impl_->publish(item, item.late, 0, uint32_t(item.late.size()));
  impl_->resident -= item.late.size();
  item.dirty = item.version != item.lateVersion;
  item.late = std::vector<uint8_t>{};
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
  impl_->flushDraws();
  require(impl_->nextFence != UINT32_MAX, "menu backend fence budget exhausted");
  require(impl_->fences.size() < 4096, "menu backend pending fence budget exceeded");
  const uint32_t fence = impl_->nextFence + 1;
  if (impl_->submitted <= impl_->completed) {
    impl_->completedFence = fence;
  } else {
    impl_->fences.emplace(fence, impl_->submitted);
  }
  impl_->nextFence = fence;
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
  impl_->flushDraws();
  for (const auto& [location, item] : impl_->resources) {
    if (item.views.contains(resource)) { return item.command > impl_->completed; }
  }
  if (impl_->uploadVersions.contains(resource)) { return false; }
  return impl_->byAddress(resource).command > impl_->completed;
}
void MenuBackend::WaitResource(uint32_t resource) { if (Busy(resource)) { Flush(); } }
bool MenuBackend::ReleaseResource(uint32_t header, uint32_t data) {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  impl_->uploadVersions.erase(header);
  auto found = std::find_if(impl_->resources.begin(), impl_->resources.end(), [&](const auto& entry) {
    return entry.second.desc.data == address(data) && entry.second.views.contains(header);
  });
  if (found == impl_->resources.end()) { return false; }
  auto& item = found->second;
  if (item.views.size() > 1) { item.views.erase(header); return false; }
  if (item.feedback) { impl_->send(webgpu::Opcode::Destroy, item.feedback); item.feedback = 0; }
  impl_->send(webgpu::Opcode::Destroy, item.id);
  impl_->resident -= item.residentBytes + item.late.size();
  impl_->resources.erase(found);
  return std::none_of(impl_->resources.begin(), impl_->resources.end(), [&](const auto& entry) {
    return entry.second.desc.data == address(data);
  });
}
void MenuBackend::WaitGpuFrame(uint64_t frame) {
  std::lock_guard lock(impl_->mutex);
  require(frame < impl_->frames, "menu backend cannot wait for unsubmitted frame");
  if (frame >= impl_->finishedFrames) { Flush(); }
}

}
