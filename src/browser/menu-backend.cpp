#include "browser/menu-backend.h"
#include "browser/render-scale.h"
#include "browser/settings-pack.h"
#include "browser/guest-vertex.h"
#include "browser/guest-primitives.h"
#include "browser/menu-shaders.h"
#include "browser/menu-texture.h"
#include "browser/render-snapshot.h"
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <type_traits>

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
struct BytesHash {
  template<class T> size_t operator()(const std::vector<T>& values) const {
    uint64_t hash = 0xcbf29ce484222325ull ^ values.size();
    const auto* bytes = reinterpret_cast<const uint8_t*>(values.data());
    const size_t size = values.size() * sizeof(T);
    size_t n = 0;
    for (; n + 8 <= size; n += 8) {
      uint64_t value;
      std::memcpy(&value, bytes + n, 8);
      hash = (hash ^ value) * 0x100000001b3ull;
      hash ^= hash >> 29;
    }
    for (; n < size; ++n) { hash = (hash ^ bytes[n]) * 0x100000001b3ull; }
    return size_t(hash ^ (hash >> 32));
  }
};
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
void convertColumn(uint32_t type, const uint8_t* first, uint32_t stride, std::span<const uint32_t> numbers, float* output, size_t pitch) {
  const auto each = [&](auto convertOne) {
    for (const uint32_t number : numbers) {
      convertOne(first + size_t(number) * stride, output);
      output += pitch;
    }
  };
  if (type == 0x72) {
    each([](const uint8_t* bytes, float* out) { std::memcpy(out, bytes, 8); std::memcpy(out + 3, bytes + 8, 4); });
    return;
  }
  const uint32_t count = type >> 4;
  switch (type & 15) {
    case 0:
      each([](const uint8_t* bytes, float* out) {
        out[0] = bytes[2] / 255.0f; out[1] = bytes[1] / 255.0f; out[2] = bytes[0] / 255.0f; out[3] = bytes[3] / 255.0f;
      });
      break;
    case 1:
      each([count](const uint8_t* bytes, float* out) {
        for (uint32_t n = 0; n < count; ++n) {
          int16_t value;
          std::memcpy(&value, bytes + n * 2, 2);
          out[n] = std::max(value / 32767.0f, -1.0f);
        }
      });
      break;
    case 5:
      each([count](const uint8_t* bytes, float* out) {
        for (uint32_t n = 0; n < count; ++n) {
          int16_t value;
          std::memcpy(&value, bytes + n * 2, 2);
          out[n] = float(value);
        }
      });
      break;
    case 2: each([count](const uint8_t* bytes, float* out) { std::memcpy(out, bytes, count * 4); }); break;
    case 4:
      each([count](const uint8_t* bytes, float* out) {
        for (uint32_t n = 0; n < count; ++n) { out[n] = bytes[n] / 255.0f; }
      });
      break;
    case 6:
      each([](const uint8_t* bytes, float* out) {
        uint32_t value;
        std::memcpy(&value, bytes, 4);
        out[0] = std::max((int32_t(value << 21) >> 21) / 1023.0f, -1.0f);
        out[1] = std::max((int32_t(value << 10) >> 21) / 1023.0f, -1.0f);
        out[2] = std::max((int32_t(value) >> 22) / 511.0f, -1.0f);
      });
      break;
  }
}
const gpu::VertexShader& layoutOf(const gpu::State& state, gpu::VertexShader& layout) {
  if (state.vs_handle & 1) {
    require(bool(state.vs), "menu backend missing programmable vertex layout");
    return *state.vs;
  }
  layout = {};
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
    case 0x308: return alpha ? 2 : 11;
    case 0x8001: case 0x8003: return 12;
    case 0x8002: case 0x8004: return 13;
    default: throw std::runtime_error("menu backend unsupported blend factor " + std::to_string(value));
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
    uint64_t feedbackUsed = 0;
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
    bool lateStarted = false;
    uint32_t lateSlot = 0;
    std::vector<uint8_t> delivered;
    std::optional<uint64_t> cpuVersion;
    // the GPU texture's size: the guest's, or more for a screen sized target at a render scale
    uint32_t hostWidth = 0, hostHeight = 0;
    uint32_t lateSource = 0;  // the texture a late readback reads: the resource, or its console sized copy
    bool multisampled = false;  // its GPU texture has a 4x companion the draws render into
    // HDR rendering (native's d3d12.cpp): hdrTarget, its texture is an HDR scene target; inHdr, it holds colour over
    // the headroom since the frame's first 3D draw, until a read resolves it; hdrFrame, the frame it entered in
    bool hdrTarget = false, inHdr = false;
    uint64_t hdrFrame = UINT64_MAX;
    bool normCube = false;  // a normalization cube by its face centres (native's UpdateNormCube)
    bool scaled() const { return hostWidth != desc.width || hostHeight != desc.height; }
  };
  Runtime& runtime;
  PreparedImage& image;
  Memory* memory;
  Execute execute;
  Execute post;
  MenuBackendLimits limits;
  ShaderInventory inventory;
  std::function<void(const FrameMetrics&)> metricsSink;
  uint64_t commandWaitMicroseconds = 0, readbackWaitMicroseconds = 0, uploadWaitMicroseconds = 0, drawWaitMicroseconds = 0;
  mutable std::recursive_mutex mutex;
  std::map<uint64_t, Resource> resources;
  struct Pipeline { uint32_t id; uint64_t used; };
  std::unordered_map<std::vector<uint8_t>, Pipeline, BytesHash> pipelines;
  uint64_t pipelineSequence = 0;
  uint64_t feedbackSequence = 0;
  std::set<uint32_t> feedbackPins;
  std::unordered_map<std::vector<uint8_t>, menu::ShaderResult, BytesHash> shaderCache;
  std::unordered_map<const menu::ShaderResult*, uint64_t> shaderSerials;
  uint64_t shaderSerial = 0, nextShaderSerial = 0;
  uint64_t shaderTranslations = 0;
  size_t shaderCacheBytes = 0;
  struct ResidentBuffer { uint32_t id, offset, bytes; std::optional<uint64_t> version; uint64_t used, token; std::list<const std::vector<uint32_t>*>::iterator order{}; };
  struct BufferPool { uint32_t id, bytes; uint64_t used; std::map<uint32_t, uint32_t> free; bool submitted = false; };
  std::map<uint32_t, BufferPool> bufferPools;
  std::vector<uint64_t> viewPins;
  std::unordered_set<uint64_t> stagedBufferViews;
  uint64_t viewSequence = 0;
  std::unordered_map<std::vector<uint32_t>, ResidentBuffer, BytesHash> residentBuffers;
  std::list<const std::vector<uint32_t>*> residentOrder;
  std::vector<uint32_t> bufferPins;
  size_t bufferBytes = 0;
  struct UploadVersion { uint32_t id; uint64_t version, used; };
  std::map<uint32_t, UploadVersion> uploadVersions;
  std::set<uint32_t> releasedResources;
  uint64_t uploadSequence = 0;
  uint32_t white = 0;
  uint32_t width = 0, height = 0;
  uint32_t canvasWidth = 0, canvasHeight = 0;
  std::function<MenuBackend::Output()> outputSource;
  float scale = 1;  // screen sized render targets are drawn this many times the guest's size
  uint32_t anisotropy = 0, brightness = 50, contrast = 50, msaa = 0;
  uint32_t softScene = 0;  // the scene a soft shadow waits to be applied to
  bool smaa = false;
  bool hdr = false, tone = false;
  uint64_t hdrResolves = 0;
  bool exposureFresh = true;
  std::chrono::steady_clock::time_point exposureTime = std::chrono::steady_clock::now();
  static constexpr float kHeadroom = 8;
  // native's HDR targets: colour targets of the back buffer's size
  bool hdrEligible(const Resource& item) const {
    return hdr && item.renderTarget && !item.depth && item.desc.width == width && item.desc.height == height && item.desc.levels == 1 &&
      !item.desc.cube && item.desc.depth == 1;
  }
  // exposure, bloom and tone mapping take an entered target back to 0..1; exposure follows the scene's over about a
  // second of wall clock
  void resolve(Resource& item) {
    if (!item.inHdr) { return; }
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::clamp(std::chrono::duration<float>(now - exposureTime).count(), 0.0f, 0.25f);
    exposureTime = now;
    const float blend = exposureFresh ? 1.0f : 1.0f - std::exp(-seconds * 1.5f);
    exposureFresh = false;
    const auto result = sendPacket(webgpu::Opcode::Resolve, item.id, webgpu::ResolvePacket{blend, uint32_t(tone)});
    item.inHdr = false; item.dirty = true; item.command = result.command; ++item.version; ++hdrResolves;
  }
  // the first depth written 3D draw of a frame into an HDR target: what it holds goes over the headroom
  void enterHdr(Resource& item) {
    item.hdrFrame = frames;
    const uint32_t copy = feedbackTexture(item);
    const std::array<float, 4> whole{0, 0, float(item.hostWidth), float(item.hostHeight)};
    const auto result = blit(copy, item.id, whole, whole, false, 1 / kHeadroom);
    item.inHdr = true; item.dirty = true; item.command = result.command; ++item.version;
  }
  // native's ShaderModel3 normalization cube test on the decoded faces: each face's centre points along its axis
  static bool normalizationCube(const gpu::TextureDesc& desc, const std::vector<uint8_t>& rgba) {
    if (!desc.cube || desc.width < 8) { return false; }
    size_t faceBytes = 0;
    for (uint32_t n = 0; n < desc.levels; ++n) { faceBytes += size_t(std::max(desc.width >> n, 1u)) * std::max(desc.height >> n, 1u) * 4; }
    if (rgba.size() < faceBytes * 6) { return false; }
    for (uint32_t face = 0; face < 6; ++face) {
      const uint8_t* centre = rgba.data() + face * faceBytes + (size_t(desc.width / 2) * desc.width + desc.width / 2) * 4;
      for (uint32_t channel = 0; channel < 3; ++channel) {
        const int want = channel == face / 2 ? (face & 1 ? 0 : 255) : 128;
        if (std::abs(int(centre[channel]) - want) > 16) { return false; }
      }
    }
    // off the centres too: the game also has a cube whose centres pass but whose texels lean towards the face's edges,
    // and drawing that one per pixel turns the opening mission's sky white. The +X face: direction (1, -t, -s)
    for (const auto [x, y] : {std::pair{desc.width / 4, desc.width / 2}, std::pair{desc.width / 2, desc.width / 4}, std::pair{desc.width / 4, desc.width / 4}}) {
      const float u = (x + 0.5f) / desc.width * 2 - 1, v = (y + 0.5f) / desc.width * 2 - 1;
      const float length = std::sqrt(1 + u * u + v * v);
      const std::array<float, 3> direction{1 / length, -v / length, -u / length};
      const uint8_t* texel = rgba.data() + (size_t(y) * desc.width + x) * 4;
      for (uint32_t channel = 0; channel < 3; ++channel) {
        if (std::abs(texel[channel] - (direction[channel] * 0.5f + 0.5f) * 255) > 16) { return false; }
      }
    }
    return true;
  }
  // native's MSAA rule: render targets of the back buffer's size, colour and depth, get the samples
  void multisample(Resource& item) {
    if (!msaa || item.multisampled || !item.renderTarget || item.desc.width != width || item.desc.height != height ||
        item.desc.levels != 1 || item.desc.cube || item.desc.depth > 1) { return; }
    sendPacket(webgpu::Opcode::Multisample, item.id, msaa);
    item.multisampled = true;
  }
  std::array<uint8_t, 768> guestRamp = [] { std::array<uint8_t, 768> ramp; for (size_t n = 0; n < ramp.size(); ++n) { ramp[n] = uint8_t(n % 256); } return ramp; }();
  void sendGamma() { send(webgpu::Opcode::SetGamma, 0, settings_pack::ComposeRamp(guestRamp.data(), brightness, contrast)); }
  uint32_t shadowResolution = 512;
  std::pair<uint32_t, uint32_t> hostSize(const gpu::TextureDesc& desc, bool target) const {
    if (!target || desc.levels != 1 || desc.cube || desc.depth > 1) { return {desc.width, desc.height}; }
    // the shadow map the game's shadows.cpp hook found: the shadow resolution in place of the render scale
    if (gpu::g_shadow.map && desc.data == address(gpu::g_shadow.map) && desc.width == 512 && desc.height == 512) {
      return {shadowResolution, shadowResolution};
    }
    return render_scale::HostSize(desc.width, desc.height, width, height, scale);
  }
  static size_t feedbackBytes(const Resource& item) { return size_t(item.hostWidth) * item.hostHeight * (item.hdrTarget ? 8 : 4); }
  // a texture of width x height drawn over from source's rectangle: box filtered when shrinking, bilinear otherwise;
  // colour times scale
  webgpu::Result blit(uint32_t source, uint32_t destination, std::array<float, 4> from, std::array<float, 4> to, bool forceOpaque = false, float scale = 1) {
    webgpu::BlitPacket packet{source, {from[0], from[1], from[2], from[3]}, {to[0], to[1], to[2], to[3]}};
    const float shrink = std::max(from[2] / to[2], from[3] / to[3]);
    packet.taps = shrink > 1.01f ? std::min(16u, uint32_t(std::ceil(shrink))) : 0;
    packet.force_opaque = forceOpaque;
    packet.scale = scale;
    return sendPacket(webgpu::Opcode::Blit, destination, packet);
  }
  uint32_t createEmpty(uint32_t w, uint32_t h, bool depth, bool hdrTarget = false) {
    const uint32_t id = newId();
    std::vector<uint8_t> payload;
    append(payload, webgpu::TexturePacket{w, h, depth ? 1u : hdrTarget ? webgpu::HdrTexture : 0u, 1});
    send(webgpu::Opcode::CreateTexture, id, payload);
    return id;
  }
  // the resource's texture at another size or format, its picture kept
  void resample(Resource& item, uint32_t w, uint32_t h) { reshape(item, w, h, item.hdrTarget); }
  void reshape(Resource& item, uint32_t w, uint32_t h, bool hdrTarget) {
    if (item.hostWidth == w && item.hostHeight == h && item.hdrTarget == hdrTarget) { return; }
    collectLate(item);
    if (!hdrTarget) { resolve(item); }
    const uint32_t id = createEmpty(w, h, item.depth, hdrTarget);
    const auto result = blit(item.id, id, {0, 0, float(item.hostWidth), float(item.hostHeight)}, {0, 0, float(w), float(h)});
    send(webgpu::Opcode::Destroy, item.id);
    if (item.feedback) {
      send(webgpu::Opcode::Destroy, item.feedback);
      const size_t bytes = feedbackBytes(item);
      resident -= std::min(resident, bytes); item.residentBytes -= std::min(item.residentBytes, bytes);
      item.feedback = 0; item.feedbackVersion = 0; item.feedbackUsed = 0;
    }
    item.id = id; item.hostWidth = w; item.hostHeight = h; item.command = result.command; ++item.version;
    item.hdrTarget = hdrTarget;
    item.multisampled = false;
    multisample(item);
  }
  // a console sized copy of a scaled target for a readback, which the caller destroys
  uint32_t shrink(Resource& item) {
    const uint32_t id = createEmpty(item.desc.width, item.desc.height, item.depth);
    blit(item.id, id, {0, 0, float(item.hostWidth), float(item.hostHeight)}, {0, 0, float(item.desc.width), float(item.desc.height)});
    return id;
  }
  // the guest rectangle in the resource's host pixels, truncated as native's ScaledRect
  std::array<int32_t, 4> scaledRect(const Resource& item, std::array<int32_t, 4> rect) const {
    if (!item.scaled()) { return rect; }
    const auto x = [&](int32_t value) { return int32_t(int64_t(value) * item.hostWidth / item.desc.width); };
    const auto y = [&](int32_t value) { return int32_t(int64_t(value) * item.hostHeight / item.desc.height); };
    return {x(rect[0]), y(rect[1]), x(rect[2]), y(rect[3])};
  }
  void applyScale(float next) {
    if (next == scale) { return; }
    scale = next;
    for (auto& [key, item] : resources) {
      if (!item.renderTarget) { continue; }
      const auto [w, h] = hostSize(item.desc, true);
      resample(item, w, h);
    }
  }
  uint32_t submitted = 0, completed = 0;
  uint64_t submittedCount = 0, completedCount = 0;
  uint32_t nextFence = 0, completedFence = 0;
  uint64_t frames = 0, finishedFrames = 0;
  uint32_t previousPresent = 0, olderPresent = 0;
  uint32_t indexSource = 0;
  std::vector<uint16_t> scratchIndices;
  std::vector<uint32_t> scratchVertices;
  std::vector<RenderResourceRequest> scratchRequests;
  std::array<uint32_t, webgpu::ReadbackSlots> lateSlots{};
  uint64_t previousPresentCount = 0, olderPresentCount = 0;
  size_t resident = 0;
  std::string storageViewFailure;
  std::vector<uint8_t> stagedDraws;
  std::set<uint32_t> stagedTargets;
  uint32_t stagedDrawCount = 0, stagedUploadCount = 0;
  std::vector<uint8_t> stagedUploads;
  std::set<uint32_t> stagedUploadPools;
  std::map<uint32_t, uint32_t> fences;
  bool initialized = false;
  bool retiring = false;
  bool retired = false;

  Impl(Runtime& owner, PreparedImage& prepared, Execute exchange, Execute posted, MenuBackendLimits budget)
    : runtime(owner), image(prepared), memory(prepared.memory.get()), execute(std::move(exchange)), post(std::move(posted)), limits(budget) {
    require(memory && execute && post && (!image.boundRuntime || image.boundRuntime == &runtime), "menu backend requires its runtime and prepared image");
  }
  void requireOwner() const {
    require(memory == image.memory.get() && (!image.boundRuntime || image.boundRuntime == &runtime), "menu backend foreign prepared image or runtime");
    require(!Memory::HasBorrow(), "menu backend cannot wait with borrowed guest memory");
    require(!retired, "menu backend is retired");
    if (!retiring && runtime.IsCancelled()) { runtime.Checkpoint({}); }
  }
  const menu::ShaderResult& shaders(const gpu::State& state, uint16_t regs, bool pretransformed,
      const gpu::TexInfo (&textures)[4], bool colorTarget, bool depthBias, const gpu::VertexShader* raw = nullptr) {
    auto key = shaderKey(state, regs, pretransformed, textures, colorTarget, depthBias);
    append(key, bool(raw));
    if (raw) { for (const auto& input : raw->inputs) { append(key, input.type); } }
    if (const auto found = shaderCache.find(key); found != shaderCache.end()) {
      shaderSerial = shaderSerials.at(&found->second);
      return found->second;
    }
    ++shaderTranslations;
    auto translated = menu::GenerateShaders(state, regs, pretransformed, textures, 0, colorTarget, depthBias);
    require(translated.supported, translated.diagnostic.c_str());
    if (raw) { translated.vertex = menu::GuestVertexInputs(std::move(translated.vertex), *raw, regs); }
    const auto bytesOf = [](const auto& key, const auto& shader) {
      return key.capacity() + shader.vertex.capacity() + shader.fragment.capacity() + shader.state.capacity() + shader.diagnostic.capacity();
    };
    const size_t bytes = bytesOf(key, translated);
    require(bytes <= (16u << 20), "menu backend shader translation budget exceeded");
    while (!shaderCache.empty() && (shaderCache.size() >= 256 || shaderCacheBytes + bytes > (16u << 20))) {
      const auto found = shaderCache.begin();
      shaderCacheBytes -= bytesOf(found->first, found->second);
      shaderSerials.erase(&found->second);
      shaderCache.erase(found);
    }
    shaderCacheBytes += bytes;
    auto& inserted = shaderCache.emplace(std::move(key), std::move(translated)).first->second;
    shaderSerial = ++nextShaderSerial;
    shaderSerials[&inserted] = shaderSerial;
    return inserted;
  }
  webgpu::Result send(webgpu::Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes = {}) {
    if (opcode == webgpu::Opcode::UploadBuffer && limits.batchDraws) {
      const size_t reserve = sizeof(webgpu::UploadDrawPacket);
      require(bytes.size() + reserve + 12 <= webgpu::MaxBytes, "menu backend staged upload exceeds packet budget");
      if (stagedDrawCount + stagedUploadCount == 256 || std::max<size_t>(stagedDraws.size(), 4) + std::max(stagedUploads.size(), reserve) + bytes.size() + 8 > webgpu::MaxBytes) { flushDraws(); }
      if (!stagedUploadCount) { stagedUploads.reserve(webgpu::MaxBytes); stagedUploads.resize(reserve); }
      append(stagedUploads, resource); append(stagedUploads, uint32_t(bytes.size())); appendBytes(stagedUploads, bytes); ++stagedUploadCount; stagedUploadPools.insert(resource);
      return {webgpu::Status::Ok, submitted, resource, {}};
    }
    if (limits.batchCommands && (opcode == webgpu::Opcode::Clear || opcode == webgpu::Opcode::Copy)) {
      uint32_t depth = 0;
      if (opcode == webgpu::Opcode::Clear) {
        require(bytes.size() >= sizeof(webgpu::ClearPacket), "menu backend truncated staged clear");
        webgpu::ClearPacket packet;
        std::memcpy(&packet, bytes.data(), sizeof(packet));
        depth = packet.depth_target;
      }
      stage(opcode, resource, depth, bytes);
      return {webgpu::Status::Ok, submitted, resource, {}};
    }
    if (opcode != webgpu::Opcode::DrawBatch && opcode != webgpu::Opcode::RenderBatch && opcode != webgpu::Opcode::UploadBuffer && opcode != webgpu::Opcode::UploadDrawBatch && opcode != webgpu::Opcode::ReadbackCollect) { flushDraws(); }
    requireOwner();
    require(bytes.size() <= webgpu::MaxBytes, "menu backend command payload budget exceeded");
    const auto started = std::chrono::steady_clock::now();
    const bool deferred = limits.postCommands && (opcode == webgpu::Opcode::Draw || opcode == webgpu::Opcode::DrawBatch || opcode == webgpu::Opcode::ReadbackStart || opcode == webgpu::Opcode::Present ||
      opcode == webgpu::Opcode::RenderBatch || opcode == webgpu::Opcode::Clear || opcode == webgpu::Opcode::Copy || opcode == webgpu::Opcode::UploadBuffer || opcode == webgpu::Opcode::UploadDrawBatch ||
      opcode == webgpu::Opcode::Resolve);
    auto result = (deferred ? post : execute)(opcode, resource, bytes);
    const uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    commandWaitMicroseconds += elapsed;
    if (opcode == webgpu::Opcode::Readback || opcode == webgpu::Opcode::ReadbackCollect) { readbackWaitMicroseconds += elapsed; }
    if (opcode == webgpu::Opcode::CreateTexture || opcode == webgpu::Opcode::CreatePipeline || opcode == webgpu::Opcode::UploadDepth || opcode == webgpu::Opcode::UploadBuffer) { uploadWaitMicroseconds += elapsed; }
    if (opcode == webgpu::Opcode::Draw || opcode == webgpu::Opcode::DrawBatch || opcode == webgpu::Opcode::RenderBatch || opcode == webgpu::Opcode::UploadDrawBatch) { drawWaitMicroseconds += elapsed; }
    if (result.status != webgpu::Status::Ok) {
      throw std::runtime_error("menu backend GPU service failure: " + std::string(result.bytes.begin(), result.bytes.end()));
    }
    require(result.resource == resource && result.command > submitted, "menu backend GPU service invalid completion identity");
    submitted = result.command;
    require(submittedCount != UINT64_MAX, "menu backend command counter budget exceeded");
    ++submittedCount;
    const bool presented = opcode == webgpu::Opcode::Present;
    if (opcode == webgpu::Opcode::Flush || opcode == webgpu::Opcode::Readback || presented) {
      if (presented) {
        completed = std::max(completed, olderPresent); completedCount = std::max(completedCount, olderPresentCount);
        finishedFrames = std::max(finishedFrames, frames ? frames - 1 : 0);
        olderPresent = previousPresent; olderPresentCount = previousPresentCount;
        previousPresent = submitted; previousPresentCount = submittedCount;
      } else { completed = submitted; completedCount = submittedCount; finishedFrames = frames; }
      for (auto found = fences.begin(); found != fences.end();) {
        if (found->second > completed) { break; }
        completedFence = found->first; found = fences.erase(found);
      }
    }
    return result;
  }
  void flushDraws() {
    if (!stagedDrawCount && !stagedUploadCount) { return; }
    if (stagedDraws.empty()) { stagedDraws.resize(4); }
    std::memcpy(stagedDraws.data(), &stagedDrawCount, 4);
    webgpu::Result result;
    if (stagedUploadCount) {
      const webgpu::UploadDrawPacket packet{stagedUploadCount, uint32_t(stagedUploads.size() - sizeof(webgpu::UploadDrawPacket)), uint32_t(stagedDraws.size()), uint32_t(limits.batchCommands)};
      std::memcpy(stagedUploads.data(), &packet, sizeof(packet)); appendBytes(stagedUploads, stagedDraws);
      result = send(webgpu::Opcode::UploadDrawBatch, 0, stagedUploads);
      for (const auto id : stagedUploadPools) { bufferPools.at(id).submitted = true; }
    } else { result = send(limits.batchCommands ? webgpu::Opcode::RenderBatch : webgpu::Opcode::DrawBatch, 0, stagedDraws); }
    for (auto& [key, item] : resources) {
      if (stagedTargets.contains(item.id)) { item.command = result.command; }
    }
    stagedDraws.clear(); stagedUploads.clear(); stagedUploadPools.clear(); stagedTargets.clear(); stagedDrawCount = 0; stagedUploadCount = 0; stagedBufferViews.clear();
  }
  std::optional<uint32_t> draw(uint32_t resource, const webgpu::DrawPacket& packet, std::span<const uint8_t> bytes) {
    if (!limits.batchDraws) { return send(webgpu::Opcode::Draw, resource, bytes).command; }
    stage(webgpu::Opcode::Draw, resource, packet.depth_target, bytes);
    return std::nullopt;
  }
  void stage(webgpu::Opcode opcode, uint32_t resource, uint32_t depth, std::span<const uint8_t> bytes) {
    requireOwner();
    const uint32_t headerBytes = limits.batchCommands ? 12 : 8;
    const size_t reserve = limits.residentVertices ? sizeof(webgpu::UploadDrawPacket) : 0;
    require(bytes.size() + headerBytes + 4 + reserve <= webgpu::MaxBytes, "menu backend staged draw exceeds packet budget");
    if (stagedDrawCount + stagedUploadCount == 256 || std::max<size_t>(stagedDraws.size(), 4) + bytes.size() + headerBytes + std::max(stagedUploads.size(), reserve) > webgpu::MaxBytes) { flushDraws(); }
    if (!stagedDrawCount) { stagedDraws.reserve(webgpu::MaxBytes); stagedDraws.resize(4); }
    if (limits.batchCommands) { append(stagedDraws, uint32_t(opcode)); }
    append(stagedDraws, resource); append(stagedDraws, uint32_t(bytes.size())); appendBytes(stagedDraws, bytes);
    if (resource) { stagedTargets.insert(resource); }
    if (depth) { stagedTargets.insert(depth); }
    ++stagedDrawCount;
  }
  template<class T> webgpu::Result sendPacket(webgpu::Opcode opcode, uint32_t resource, const T& packet) {
    std::vector<uint8_t> bytes;
    append(bytes, packet);
    return send(opcode, resource, bytes);
  }
  template<class Capture = std::nullptr_t> ResidentBuffer residentBuffer(std::vector<uint32_t> key, uint32_t location, uint32_t size,
      Capture capture = nullptr, uint32_t headerAddress = 0, uint32_t sourceOffset = 0) {
    auto found = residentBuffers.end();
    std::optional<uint64_t> version;
    std::vector<uint8_t> bytes;
    const auto touch = [&](auto entry) {
      auto& view = entry->second;
      if (view.used) { residentOrder.splice(residentOrder.end(), residentOrder, view.order); }
      else { view.order = residentOrder.insert(residentOrder.end(), &entry->first); }
      view.used = ++uploadSequence;
      bufferPools.at(view.id).used = view.used;
      bufferPins.push_back(view.id); viewPins.push_back(view.token);
      return view;
    };
    if (!headerAddress && !location) {
      found = residentBuffers.find(key); version = 1;
      if (found != residentBuffers.end() && found->second.version == version) { return touch(found); }
      if constexpr (std::is_same_v<Capture, std::nullptr_t>) { throw std::runtime_error("missing index snapshot capture"); }
      else { bytes = capture(); }
    } else {
      auto lease = memory->Borrow();
      if (headerAddress) {
        std::array<uint8_t, 12> header{};
        memory->Read({}, headerAddress, header.data(), header.size());
        require((word(header, 0) & 0x70000) == 0, "menu backend incompatible vertex buffer format");
        const uint32_t data = address(word(header, 4));
        require(data && uint64_t(data) + sourceOffset + size <= (uint64_t(1) << 32), "resident vertex range overflow");
        location = data + sourceOffset; key[0] = location;
      }
      found = residentBuffers.find(key);
      version = location ? memory->PhysicalWriteVersion({}, location, size) : std::optional<uint64_t>(1);
      if (found != residentBuffers.end() && version && found->second.version == version) { return touch(found); }
      bytes = read(location, size);
    }
    require(bytes.size() <= webgpu::MaxBytes - sizeof(webgpu::BufferUpload) - sizeof(webgpu::UploadDrawPacket) - 12, "resident buffer upload budget exceeded");
    const auto upload = [&](uint32_t id, uint32_t offset, uint32_t capacity) {
      std::vector<uint8_t> packet;
      append(packet, webgpu::BufferUpload{offset, capacity}); appendBytes(packet, bytes);
      send(webgpu::Opcode::UploadBuffer, id, packet);
    };
    if (found != residentBuffers.end()) {
      if (stagedBufferViews.contains(found->second.token)) { flushDraws(); }
      upload(found->second.id, found->second.offset, bufferPools.at(found->second.id).bytes);
      found->second.version = version;
      return touch(found);
    }
    const uint32_t storageBytes = uint32_t((bytes.size() + 3) & ~size_t(3));
    const auto evict = [&] {
      auto oldest = residentBuffers.end();
      for (const auto* key : residentOrder) {
        const auto entry = residentBuffers.find(*key);
        if (std::find(viewPins.begin(), viewPins.end(), entry->second.token) == viewPins.end()) { oldest = entry; break; }
      }
      if (oldest == residentBuffers.end()) { return false; }
      residentOrder.erase(oldest->second.order);
      if (stagedBufferViews.contains(oldest->second.token)) { flushDraws(); }
      auto& free = bufferPools.at(oldest->second.id).free;
      uint32_t offset = oldest->second.offset, length = oldest->second.bytes;
      auto next = free.lower_bound(offset);
      if (next != free.begin()) {
        const auto previous = std::prev(next);
        if (previous->first + previous->second == offset) { offset = previous->first; length += previous->second; free.erase(previous); }
      }
      if (next != free.end() && offset + length == next->first) { length += next->second; free.erase(next); }
      free.emplace(offset, length); residentBuffers.erase(oldest);
      return true;
    };
    const size_t maximumViews = size_t(std::min<uint64_t>(16384, uint64_t(limits.maxResources) * 32));
    require(maximumViews, "resident buffer cache disabled");
    if (residentBuffers.size() >= maximumViews) { require(evict(), "resident buffer pinned view budget exceeded"); }
    const auto allocate = [&]() -> std::optional<std::pair<uint32_t, uint32_t>> {
      for (auto& [id, pool] : bufferPools) {
        for (auto range = pool.free.begin(); range != pool.free.end(); ++range) {
          if (range->second < storageBytes) { continue; }
          const uint32_t offset = range->first, remainder = range->second - storageBytes;
          pool.free.erase(range);
          if (remainder) { pool.free.emplace(offset + storageBytes, remainder); }
          return std::pair{id, offset};
        }
      }
      return std::nullopt;
    };
    auto allocation = allocate();
    const uint32_t remaining = uint32_t((limits.maxResidentBytes - std::min<size_t>(resident, limits.maxResidentBytes)) & ~size_t(3));
    const uint32_t capacity = std::max(storageBytes, std::min(remaining, (std::min(1u << 20, limits.maxResourceBytes) + 3) & ~3u));
    const auto available = [&] { return resourceCount() < limits.maxResources && resident + capacity <= limits.maxResidentBytes; };
    while (!allocation && !available() && evict()) { allocation = allocate(); }
    if (!allocation) {
      trimFeedback(1, capacity);
      require(available(), "resident buffer pool budget exceeded");
      const uint32_t id = newId();
      upload(id, 0, capacity);
      BufferPool pool{id, capacity, 0, {}, !limits.batchDraws};
      if (capacity > storageBytes) { pool.free.emplace(storageBytes, capacity - storageBytes); }
      bufferPools.emplace(id, std::move(pool)); bufferBytes += capacity; resident += capacity;
      allocation = std::pair{id, 0u};
    } else { upload(allocation->first, allocation->second, bufferPools.at(allocation->first).bytes); }
    require(viewSequence != UINT64_MAX, "resident buffer view identity exhausted");
    auto inserted = residentBuffers.emplace(std::move(key), ResidentBuffer{allocation->first, allocation->second, storageBytes, version, 0, ++viewSequence}).first;
    return touch(inserted);
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
    if ((desc.depth > 1 && (desc.cube || desc.linear)) || (desc.cube && (desc.width != desc.height || desc.linear))) {
      throw std::runtime_error("menu backend unsupported texture dimension: format=" + std::to_string(format) + " width=" + std::to_string(desc.width) +
        " height=" + std::to_string(desc.height) + " depth=" + std::to_string(desc.depth) + " levels=" + std::to_string(desc.levels) + " cube=" + std::to_string(desc.cube));
    }
    require(desc.width <= 4096 && desc.height <= 4096 && desc.depth <= 2048 &&
      (desc.depth == 1 || (desc.width <= 2048 && desc.height <= 2048)), "menu backend texture dimension budget exceeded");
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
    return bufferPools.size() + resources.size() + size_t(white != 0) + std::count_if(resources.begin(), resources.end(), [](const auto& entry) { return entry.second.feedback != 0; });
  }
  void trimFeedback(size_t slots, size_t bytes, const Resource* retained = nullptr) {
    if (slots > limits.maxResources || bytes > limits.maxResidentBytes) { return; }
    while (resourceCount() + slots > limits.maxResources || resident + bytes > limits.maxResidentBytes) {
      auto buffer = bufferPools.end();
      for (auto entry = bufferPools.begin(); entry != bufferPools.end(); ++entry) {
        if (std::find(bufferPins.begin(), bufferPins.end(), entry->first) == bufferPins.end() && (buffer == bufferPools.end() || entry->second.used < buffer->second.used)) { buffer = entry; }
      }
      if (buffer != bufferPools.end()) {
        send(webgpu::Opcode::Destroy, buffer->first); resident -= buffer->second.bytes; bufferBytes -= buffer->second.bytes;
        std::erase_if(residentBuffers, [&](const auto& view) {
          if (view.second.id != buffer->first) { return false; }
          if (view.second.used) { residentOrder.erase(view.second.order); }
          return true;
        });
        bufferPools.erase(buffer); continue;
      }

      auto oldest = resources.end();
      for (auto found = resources.begin(); found != resources.end(); ++found) {
        const auto& candidate = found->second;
        if (&candidate != retained && candidate.feedback && !feedbackPins.contains(candidate.feedback) &&
            (oldest == resources.end() || candidate.feedbackUsed < oldest->second.feedbackUsed)) { oldest = found; }
      }
      if (oldest == resources.end()) { return; }
      auto& retired = oldest->second;
      send(webgpu::Opcode::Destroy, retired.feedback);
      const size_t retiredBytes = feedbackBytes(retired);
      resident -= retiredBytes; retired.residentBytes -= retiredBytes;
      retired.feedback = 0; retired.feedbackVersion = 0; retired.feedbackUsed = 0;
    }
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
        item.desc.depth == desc.depth && item.desc.pitch == desc.pitch && item.desc.levels == desc.levels && item.desc.cube == desc.cube && item.desc.linear == desc.linear && item.uploaded.size() == bytes;
      if (!compatible) {
        collectLate(item);
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
        if (item.scaled() || item.hdrTarget) { return over(item, std::move(source), cpuVersion); }
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
    const bool normCube = hdr && normalizationCube(desc, rgba);
    const size_t residentBytes = size_t(bytes) + (depth ? size_t(desc.width) * desc.height * 4 : rgba.size());
    const size_t oldBytes = found == resources.end() ? 0 : found->second.residentBytes + found->second.late.size();
    trimFeedback(found == resources.end() ? 1 : 0, residentBytes > oldBytes ? residentBytes - oldBytes : 0,
      found == resources.end() ? nullptr : &found->second);
    require(resourceCount() < limits.maxResources || found != resources.end(), "menu backend resource count budget exceeded");
    require(residentBytes <= limits.maxResidentBytes && resident - std::min(resident, oldBytes) <= limits.maxResidentBytes - residentBytes, ("menu backend resident byte budget exceeded: resident=" + std::to_string(resident) + " replacement=" + std::to_string(oldBytes) + " request=" + std::to_string(residentBytes) + " limit=" + std::to_string(limits.maxResidentBytes)).c_str());
    const uint32_t id = newId();
    webgpu::TexturePacket packet{desc.width, desc.height, depth ? 1u : desc.cube ? 2u : desc.depth > 1 ? 3u : 0u, desc.levels};
    std::vector<uint8_t> payload;
    append(payload, packet);
    if (desc.depth > 1) { append(payload, desc.depth); }
    appendBytes(payload, rgba);
    const auto [hostWidth, hostHeight] = hostSize(desc, surface || (found != resources.end() && found->second.renderTarget));
    webgpu::Result result;
    if (hostWidth == desc.width && hostHeight == desc.height) {
      result = send(webgpu::Opcode::CreateTexture, id, payload);
    } else if (depth) {
      payload.clear();
      append(payload, webgpu::TexturePacket{hostWidth, hostHeight, 1u, 1});
      result = send(webgpu::Opcode::CreateTexture, id, payload);
    } else {
      const uint32_t staging = newId();
      send(webgpu::Opcode::CreateTexture, staging, payload);
      payload.clear();
      append(payload, webgpu::TexturePacket{hostWidth, hostHeight, 0u, 1});
      send(webgpu::Opcode::CreateTexture, id, payload);
      result = blit(staging, id, {0, 0, float(desc.width), float(desc.height)}, {0, 0, float(hostWidth), float(hostHeight)});
      send(webgpu::Opcode::Destroy, staging);
    }
    uint64_t version = 1;
    if (found != resources.end()) {
      version = found->second.version + 1;
      collectLate(found->second);
      send(webgpu::Opcode::Destroy, found->second.id);
      if (found->second.feedback) { send(webgpu::Opcode::Destroy, found->second.feedback); }
      resident -= std::min(resident, oldBytes);
    }
    Resource item;
    item.id = id; item.version = version; item.command = result.command; item.cpuVersion = cpuVersion;
    item.desc = desc; item.headerAddress = headerAddress; item.header = std::move(header); item.uploaded = std::move(source); item.depth = depth; item.residentBytes = residentBytes;
    item.hostWidth = hostWidth; item.hostHeight = hostHeight; item.normCube = normCube;
    if (found != resources.end() && !replacedStorage) { item.views = std::move(found->second.views); item.renderTarget = found->second.renderTarget; }
    item.views.insert(headerAddress);
    resources[key] = std::move(item);
    resident += residentBytes;
    auto& stored = resources.at(key);
    observeView(stored, headerAddress);
    return stored;
  }
  // CPU writes into a scaled target: the rows that changed are drawn over it from a console sized upload, so the rest
  // of the high resolution picture stays (native's UploadTarget after a Sync)
  Resource& over(Resource& item, std::vector<uint8_t> source, std::optional<uint64_t> cpuVersion) {
    uint32_t first = 0, last = item.desc.height;
    if (item.desc.linear && item.desc.pitch && source.size() == item.uploaded.size()) {
      const auto differs = [&](uint32_t row) {
        const size_t begin = size_t(row) * item.desc.pitch, end = std::min(begin + item.desc.pitch, source.size());
        return !std::equal(source.begin() + begin, source.begin() + end, item.uploaded.begin() + begin);
      };
      while (first < last && !differs(first)) { ++first; }
      while (last > first && !differs(last - 1)) { --last; }
    }
    if (first < last) {
      auto decoded = menu::DecodeTexture(item.desc, source, {}, 0, 0);
      require(decoded.supported, decoded.diagnostic.c_str());
      const uint32_t staging = newId();
      std::vector<uint8_t> payload;
      append(payload, webgpu::TexturePacket{item.desc.width, item.desc.height, 0, 1});
      appendBytes(payload, decoded.pixels);
      send(webgpu::Opcode::CreateTexture, staging, payload);
      const float ky = float(item.hostHeight) / item.desc.height;
      const auto result = blit(staging, item.id, {0, float(first), float(item.desc.width), float(last - first)},
        {0, std::floor(first * ky), float(item.hostWidth), std::ceil(last * ky) - std::floor(first * ky)}, false, item.inHdr ? 1 / kHeadroom : 1);
      send(webgpu::Opcode::Destroy, staging);
      item.command = result.command; ++item.version;
    }
    item.uploaded = std::move(source); item.cpuVersion = cpuVersion;
    return item;
  }
  Resource& surface(const gpu::Surface& value, bool depth = false) {
    auto& item = ensure(value.header, true, depth);
    if (!item.renderTarget) {
      item.renderTarget = true;
      const auto [w, h] = hostSize(item.desc, true);
      reshape(item, w, h, hdrEligible(item));
    }
    multisample(item);
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
    require(feedbackSequence != UINT64_MAX, "menu backend feedback cache sequence exhausted");
    if (!item.feedback) {
      const size_t bytes = feedbackBytes(item);
      require(bytes <= limits.maxResourceBytes && bytes <= limits.maxResidentBytes, "menu backend feedback resident byte budget exceeded");
      trimFeedback(1, bytes, &item);
      require(resourceCount() < limits.maxResources, ("menu backend feedback resource count budget exceeded: originals=" + std::to_string(resources.size()) +
        " feedback=" + std::to_string(resourceCount() - resources.size() - size_t(white != 0)) + " pinned=" + std::to_string(feedbackPins.size()) +
        " limit=" + std::to_string(limits.maxResources)).c_str());
      require(bytes <= limits.maxResourceBytes && bytes <= limits.maxResidentBytes && resident <= limits.maxResidentBytes - bytes, "menu backend feedback resident byte budget exceeded");
      std::vector<uint8_t> payload;
      append(payload, webgpu::TexturePacket{item.hostWidth, item.hostHeight, item.hdrTarget ? webgpu::HdrTexture : 0u});
      const uint32_t id = newId();
      send(webgpu::Opcode::CreateTexture, id, payload);
      item.feedback = id; item.residentBytes += bytes; resident += bytes;
    }
    item.feedbackUsed = ++feedbackSequence;
    feedbackPins.insert(item.feedback);
    if (item.feedbackVersion != item.version) {
      sendPacket(webgpu::Opcode::Copy, item.feedback, webgpu::CopyPacket{item.id, item.hostWidth, item.hostHeight});
      item.feedbackVersion = item.version;
    }
    return item.feedback;
  }
  uint32_t whiteTexture() {
    if (!white) {
      trimFeedback(1, 8);
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
    resolve(item);
    if (!item.scaled() && !item.hdrTarget) { return decodeReadback(item, send(webgpu::Opcode::Readback, item.id)); }
    const uint32_t copy = shrink(item);
    auto result = send(webgpu::Opcode::Readback, copy);
    send(webgpu::Opcode::Destroy, copy);
    return decodeReadback(item, result);
  }
  bool startLate(Resource& item) {
    const auto free = std::find(lateSlots.begin(), lateSlots.end(), 0u);
    if (free == lateSlots.end()) { return false; }
    const uint32_t slot = uint32_t(free - lateSlots.begin());
    std::vector<uint8_t> request;
    append(request, slot);
    resolve(item);
    item.lateSource = item.scaled() || item.hdrTarget ? shrink(item) : item.id;
    send(webgpu::Opcode::ReadbackStart, item.lateSource, request);
    *free = item.lateSource;
    item.lateStarted = true; item.lateSlot = slot;
    return true;
  }
  bool collectLate(Resource& item, bool poll = false) {
    if (!item.lateStarted) { return true; }
    std::vector<uint8_t> request;
    append(request, item.lateSlot); append(request, uint32_t(poll));
    const auto started = std::chrono::steady_clock::now();
    const auto result = execute(webgpu::Opcode::ReadbackCollect, item.lateSource, request);
    const uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    commandWaitMicroseconds += elapsed; readbackWaitMicroseconds += elapsed;
    if (result.status != webgpu::Status::Ok) {
      throw std::runtime_error("menu backend GPU service failure: " + std::string(result.bytes.begin(), result.bytes.end()));
    }
    if (result.bytes.empty()) {
      require(poll, "menu backend empty asynchronous readback");
      return false;
    }
    lateSlots.at(item.lateSlot) = 0;
    item.lateStarted = false;
    if (item.lateSource != item.id) { send(webgpu::Opcode::Destroy, item.lateSource); }
    auto bytes = decodeReadback(item, result);
    resident = resident - item.late.size() + bytes.size();
    item.late = std::move(bytes);
    return true;
  }
  std::vector<uint8_t> decodeReadback(Resource& item, const webgpu::Result& result) {
    if (item.depth) {
      require(result.bytes.size() == uint64_t(item.desc.width) * item.desc.height * 4, "menu backend depth readback size mismatch");
      auto bytes = item.uploaded;
      const uint32_t pixelBytes = item.desc.format == 0x30 ? 2 : 4;
      for (uint32_t n = 0; n < item.desc.width * item.desc.height; ++n) {
        uint32_t value = word(result.bytes, n * 4);
        if (pixelBytes == 2) { value = uint32_t((uint64_t(value >> 8) * 65535 + 8388607) / 16777215); }
        std::memcpy(bytes.data() + n / item.desc.width * item.desc.pitch + n % item.desc.width * pixelBytes, &value, pixelBytes);
      }
      return bytes;
    }
    auto encoded = menu::EncodeSurface(item.desc, item.uploaded, result.bytes);
    require(encoded.supported, encoded.diagnostic.c_str());
    return std::move(encoded.bytes);
  }

  void publish(Resource& item, std::span<const uint8_t> bytes, uint32_t offset, uint32_t size) {
    memory->CheckRange({}, item.desc.data + offset, size);
    const uint32_t rows = item.desc.linear ? item.desc.height : 1;
    const size_t pitch = item.desc.linear ? item.desc.pitch : 0;
    const size_t rowBytes = item.desc.linear ? size_t(item.desc.width) * (item.depth && item.desc.format == 0x30 ? 2 : 4) : item.uploaded.size();
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
  : impl_(std::make_unique<Impl>(runtime, image, [transport](auto opcode, auto resource, auto bytes) {
    require(bool(transport), "menu backend missing injected GPU transport");
    return transport->Execute(opcode, resource, bytes);
  }, [transport](auto opcode, auto resource, auto bytes) {
    require(bool(transport), "menu backend missing injected GPU transport");
    return transport->Post(opcode, resource, bytes);
  }, limits)) {}
MenuBackend::MenuBackend(Runtime& runtime, PreparedImage& image, Execute execute, MenuBackendLimits limits)
  : impl_(std::make_unique<Impl>(runtime, image, execute, execute, limits)) {}
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
  impl_->stagedDraws.clear(); impl_->stagedUploads.clear(); impl_->stagedUploadPools.clear(); impl_->stagedTargets.clear(); impl_->stagedDrawCount = 0; impl_->stagedUploadCount = 0;
  impl_->retiring = true;
  struct RetirementScope {
    bool& active;
    ~RetirementScope() { active = false; }
  } scope{impl_->retiring};
  while (!impl_->pipelines.empty()) {
    const auto found = impl_->pipelines.begin();
    impl_->send(webgpu::Opcode::Destroy, found->second.id);
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
  impl_->shaderCache.clear(); impl_->shaderSerials.clear(); impl_->shaderCacheBytes = 0;
  for (const auto& entry : impl_->bufferPools) { if (entry.second.submitted) { impl_->send(webgpu::Opcode::Destroy, entry.first); } }
  impl_->bufferPools.clear();
  impl_->residentBuffers.clear(); impl_->residentOrder.clear(); impl_->bufferPins.clear(); impl_->viewPins.clear(); impl_->stagedBufferViews.clear(); impl_->bufferBytes = 0;
  impl_->uploadVersions.clear();
  impl_->releasedResources.clear();
  impl_->initialized = false;
  impl_->retired = true;
}
MenuBackend::SnapshotData MenuBackend::Snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return {uint32_t(impl_->pipelines.size()), uint32_t(impl_->resourceCount()), impl_->resident,
    impl_->submittedCount, impl_->completedCount, impl_->frames, impl_->storageViewFailure,
    impl_->shaderTranslations, uint32_t(impl_->shaderCacheBytes), uint32_t(impl_->shaderCache.size()), uint32_t(impl_->uploadVersions.size() + impl_->residentBuffers.size()),
    impl_->hdr, impl_->hdrResolves};
}
bool MenuBackend::Init(uint32_t width, uint32_t height) {
  std::lock_guard lock(impl_->mutex);
  require(!impl_->initialized, "menu backend already initialized");
  require(width && height && width <= 4096 && height <= 4096, "menu backend target dimension budget exceeded");
  impl_->sendPacket(webgpu::Opcode::Configure, 0, webgpu::Dimensions{width, height});
  impl_->width = width; impl_->height = height; impl_->initialized = true;
  if (impl_->outputSource) {
    const auto output = impl_->outputSource();
    impl_->msaa = output.msaa == 4 ? 4 : 0;
    impl_->hdr = output.hdrRendering;
  }
  impl_->canvasWidth = width; impl_->canvasHeight = height;
  return true;
}
void MenuBackend::IndexSource(uint32_t address) {
  std::lock_guard lock(impl_->mutex);
  impl_->indexSource = address;
}
void MenuBackend::Draw(const gpu::State& input, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
  std::lock_guard lock(impl_->mutex);
  const uint32_t indexSource = std::exchange(impl_->indexSource, 0);
  impl_->requireOwner();
  impl_->feedbackPins.clear();
  impl_->bufferPins.clear(); impl_->viewPins.clear();
  require(impl_->initialized, "menu backend draw before initialization");
  require(count <= impl_->limits.maxVertices, "menu backend vertex budget exceeded");
  impl_->checkState(input);
  const auto expandedCount = menu::ExpandedIndexCount(prim, count);
  require(expandedCount <= (impl_->limits.maxExpandedVertices ? impl_->limits.maxExpandedVertices : impl_->limits.maxVertices), "menu backend expanded vertex budget exceeded");
  if (!expandedCount) { return; }
  require(!input.vs || (input.vs->decl.size() <= 65536 && input.vs->program.size() <= 65536 && input.vs->constants.size() <= 192), "menu backend shader budget exceeded");
  gpu::VertexShader fixedLayout;
  const auto& layout = layoutOf(input, fixedLayout);
  const auto& budget = impl_->limits;
  require(layout.decl.size() <= 65536 && layout.program.size() <= 65536 && layout.constants.size() <= 192, "menu backend shader budget exceeded");
  auto& indexValues = impl_->scratchIndices;
  indexValues.clear();
  if (indices) {
    indexValues.resize(count);
    std::memcpy(indexValues.data(), indices, size_t(count) * 2);
  }
  auto& vertexNumbers = impl_->scratchVertices;
  vertexNumbers.resize(count);
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
  bool raw = budget.residentVertices;
  webgpu::RawLayout rawLayout{};
  uint32_t rawMask = regs;
  for (uint32_t n = 0; n < 16 && raw; ++n) {
    if (!(regs & (1u << n))) { continue; }
    const auto& element = layout.inputs[n];
    const auto& stream = input.streams[size_t(element.stream)];
    const auto format = menu::GuestVertex(element.type);
    const uint32_t stride = stream.stride ? stream.stride : layout.stream_stride[size_t(element.stream)];
    const uint32_t alignment = std::min(format.extraFormat ? format.extraOffset : format.bytes, 4u);
    if (element.stream >= 8 || stride > 2048 || !format.format || stride % 4 || !alignment || element.offset % alignment || element.offset + format.bytes > stride || streamBytes[size_t(element.stream)] > webgpu::MaxBytes - sizeof(webgpu::BufferUpload) - sizeof(webgpu::UploadDrawPacket) - 12) { raw = false; break; }
    rawLayout.strides[element.stream] = stride;
    rawLayout.attributes[n] = {uint32_t(element.stream), element.offset, format.format};
    if (format.extraFormat) {
      uint32_t location = 0;
      while (location < 16 && (rawMask & (1u << location))) { ++location; }
      if (location == 16) { raw = false; break; }
      rawMask |= 1u << location;
      rawLayout.attributes[location] = {uint32_t(element.stream), element.offset + format.extraOffset, format.extraFormat};
    }
  }

  auto& requests = impl_->scratchRequests;
  requests.clear();
  const auto requestAt = [&](uint32_t header) -> std::optional<size_t> {
    for (size_t n = 0; n < requests.size(); ++n) {
      if (requests[n].headerAddress == header) { return n; }
    }
    return std::nullopt;
  };
  const auto addRequest = [&](RenderResourceKind kind, uint32_t header, uint32_t data, uint32_t bytes, uint64_t version, uint32_t gpuId = 0) {
    if (header) { impl_->releasedResources.erase(header); }
    if (kind == RenderResourceKind::Texture || kind == RenderResourceKind::Surface) {
      kind = kind == RenderResourceKind::Texture ? RenderResourceKind::TextureReference : RenderResourceKind::SurfaceReference;
      bytes = 0;
    }
    if (const auto existing = header ? requestAt(header) : std::nullopt) {
      auto& request = requests.at(*existing);
      require(request.kind == kind && request.payloadAddress == data, "menu backend incompatible shared resource binding");
      request.payloadBytes = std::max(request.payloadBytes, bytes);
      return;
    }
    uint32_t id = 0;
    if (kind == RenderResourceKind::TextureReference || kind == RenderResourceKind::SurfaceReference) {
      require(gpuId, "menu backend missing snapshot GPU storage identity");
      id = gpuId;
    } else {
      const uint32_t identity = header ? header : data;
      auto found = impl_->uploadVersions.find(identity);
      if (found == impl_->uploadVersions.end()) {
        require(budget.maxResources, "menu backend upload resource cache is disabled");
        if (impl_->uploadVersions.size() >= budget.maxResources) {
          const auto oldest = std::min_element(impl_->uploadVersions.begin(), impl_->uploadVersions.end(), [](const auto& left, const auto& right) {
            return left.second.used < right.second.used;
          });
          impl_->uploadVersions.erase(oldest);
        }
        found = impl_->uploadVersions.emplace(identity, Impl::UploadVersion{impl_->newId(), 0, 0}).first;
      }
      require(found->second.version != UINT64_MAX && impl_->uploadSequence != UINT64_MAX, "menu backend upload version budget exceeded");
      found->second.used = ++impl_->uploadSequence;
      id = found->second.id; version = ++found->second.version;
    }
    requests.push_back({id, version, kind, header, data, bytes});
  };
  for (uint32_t n = 0; n < 16; ++n) {
    const auto& stream = input.streams[n];
    if (!stream.vb || raw) { continue; }
    auto header = impl_->read(stream.vb, 12);
    require((word(header, 0) & 0x70000) == 0, "menu backend incompatible vertex buffer format");
    addRequest(RenderResourceKind::VertexBuffer, stream.vb, address(word(header, 4)), std::max(streamBytes[n], 1u), 1);
  }
  const bool depthOnly = !input.rt.data && !input.simple[gpu::kMColorWriteEnable / 4] && input.zs.header &&
    (input.rs[gpu::kRsZEnable] || input.rs[gpu::kRsStencilEnable]);
  // targets first: one sampled before it was drawn to is promoted to its render scale before any texture of this draw
  // (its feedback copy included) is bound
  if (!depthOnly) { impl_->surface(input.rt); }
  if (input.zs.header && (input.rs[gpu::kRsZEnable] || input.rs[gpu::kRsStencilEnable])) { impl_->surface(input.zs, true); }
  // native's EnterHdrForScene: screen quads never start it, so loading fades draw over the last tone mapped picture
  if (!depthOnly && input.zs.header && input.rs[gpu::kRsZEnable] && input.simple[gpu::kMZWriteEnable / 4] &&
      !(!(input.vs_handle & 1) && (input.vs_handle & 14) == 4)) {
    auto& scene = impl_->surface(input.rt);
    if (scene.hdrTarget && !scene.inHdr && scene.hdrFrame != impl_->frames) { impl_->enterHdr(scene); }
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
      texinfo[n].kind = item.desc.cube ? gpu::kTexCube : item.desc.depth > 1 ? gpu::kTex3D : gpu::kTex2D; texinfo[n].rect = item.desc.linear;
      if (item.inHdr) { texinfo[n].flags |= gpu::kTexHdr; }
      if (item.normCube && impl_->hdr) { texinfo[n].flags |= gpu::kTexNormCube; }
      texsize[n] = {float(item.desc.width), float(item.desc.height), item.renderTarget ? -0.5f / item.desc.width : 0, item.renderTarget ? -0.5f / item.desc.height : 0};
      packet.texture[n] = input.rt.data && item.desc.data == address(input.rt.data) ? impl_->feedbackTexture(item) : item.id;
      addRequest(RenderResourceKind::Texture, input.textures[n], item.desc.data, uint32_t(item.uploaded.size()), item.version, item.id);
      const auto& sampling = input.tss[n];
      packet.sampler[n][0] = samplerAddress(sampling[gpu::kTssAddressU]);
      if (item.desc.depth > 1) { packet.address_w[n] = samplerAddress(sampling[gpu::kTssAddressW]); }
      packet.sampler[n][1] = samplerAddress(sampling[gpu::kTssAddressV]);
      const uint32_t mag = std::min(sampling[gpu::kTssMagFilter], 2u), min = std::min(sampling[gpu::kTssMinFilter], 2u), mip = sampling[gpu::kTssMipFilter];
      require(mag >= 1 && min >= 1 && mip <= 2 && sampling[gpu::kTssMagFilter] <= 3 && sampling[gpu::kTssMinFilter] <= 3, "menu backend unsupported sampler filter");
      const bool border = sampling[gpu::kTssAddressU] == 4 || sampling[gpu::kTssAddressV] == 4;
      packet.anisotropy[n] = settings_pack::Anisotropy(impl_->anisotropy, sampling[gpu::kTssMinFilter], sampling[gpu::kTssMagFilter], mip,
        sampling[gpu::kTssMaxAnisotropy], item.desc.levels, texinfo[n].shadow != 0, border);
      const float bias = std::bit_cast<float>(sampling[gpu::kTssMipLodBias]);
      require(std::isfinite(bias), "menu backend unsupported nonfinite sampler LOD bias");
      require((item.desc.levels == 1 && min == mag) || (bias >= -16 && bias < 16), "menu backend sampler LOD bias exceeds WebGPU range");
      packet.sampler[n][2] = mag; packet.sampler[n][3] = min; packet.sampler[n][4] = mip == 2 ? 2 : 1;
      if (packet.anisotropy[n] > 1) { packet.sampler[n][2] = packet.sampler[n][3] = packet.sampler[n][4] = 2; }
      packet.sampler[n][5] = std::min(sampling[gpu::kTssMaxMipLevel], item.desc.levels - 1);
      packet.sampler[n][6] = mip ? item.desc.levels - 1 : packet.sampler[n][5];
    } else {
      packet.texture[n] = impl_->whiteTexture();
      std::fill_n(packet.sampler[n], 5, 1);
    }
  }

  auto* colorTarget = depthOnly ? nullptr : &impl_->surface(input.rt);
  if (colorTarget) { addRequest(RenderResourceKind::Surface, input.rt.header, colorTarget->desc.data, uint32_t(colorTarget->uploaded.size()), colorTarget->version, colorTarget->id); }
  Impl::Resource* depthTarget = nullptr;
  if (input.zs.header && (input.rs[gpu::kRsZEnable] || input.rs[gpu::kRsStencilEnable])) {
    depthTarget = &impl_->surface(input.zs, true);
    require(!colorTarget || (depthTarget->desc.width == colorTarget->desc.width && depthTarget->desc.height == colorTarget->desc.height &&
      depthTarget->hostWidth == colorTarget->hostWidth && depthTarget->hostHeight == colorTarget->hostHeight), "menu backend incompatible depth target dimensions");
    impl_->initializeDepth(*depthTarget);
    packet.depth_target = depthTarget->id;
    addRequest(RenderResourceKind::Surface, input.zs.header, depthTarget->desc.data, uint32_t(depthTarget->uploaded.size()), depthTarget->version, depthTarget->id);
  }
  require(colorTarget || depthTarget, "menu backend draw requires a color or depth target");
  auto& target = colorTarget ? *colorTarget : *depthTarget;
  if (input.index_buffer) {
    auto lease = impl_->memory->Borrow();
    auto header = impl_->read(input.index_buffer, 12);
    const uint32_t data = address(word(header, 4));
    require(!input.index_data || address(input.index_data) == data, "menu backend incompatible index buffer data binding");
    if (raw) {
      require((word(header, 0) & 0x70000) == 0x10000, "menu backend incompatible index buffer format");
      impl_->memory->CheckRange({}, data, indices ? count * 2 : 1);
    }
    else { addRequest(RenderResourceKind::IndexBuffer, input.index_buffer, data, indices ? count * 2 : 1, 1); }
  } else if (input.index_data) {
    if (raw) { impl_->memory->CheckRange({}, address(input.index_data), indices ? count * 2 : 1); }
    else { addRequest(RenderResourceKind::IndexData, 0, address(input.index_data), indices ? count * 2 : 1, 1); }
  }
  RenderSnapshotLimits snapshotLimits;
  snapshotLimits.maxResources = budget.maxResources; snapshotLimits.maxPayloadBytes = budget.maxResourceBytes;
  snapshotLimits.maxTotalBytes = budget.maxResidentBytes;
  const auto captured = CaptureRenderResources({}, *impl_->memory, input, requests, snapshotLimits, {colorTarget != nullptr, packet.depth_target != 0, !raw});
  const auto& state = input;
  const bool pretransformed = !(state.vs_handle & 1) && (state.vs_handle & 14) == 4;
  const bool depthBias = packet.depth_target && prim >= 5 && state.simple[gpu::kMSolidOffsetEnable / 4];
  const auto& shaders = impl_->shaders(state, regs, pretransformed, texinfo, colorTarget != nullptr, depthBias, raw ? &layout : nullptr);
  impl_->inventory.Observe(state, pretransformed, texinfo);
  for (uint32_t n = 0; n < 4; ++n) {
    if (state.textures[n]) { impl_->inventory.Texture(textureFormats[n]); }
  }
  webgpu::PipelinePacket pipeline{};
  pipeline.vertex_mask = raw ? rawMask | webgpu::ResidentVertexMask : budget.compactVertices ? regs : 0;
  pipeline.color_target = colorTarget != nullptr;
  pipeline.color_format = colorTarget && colorTarget->hdrTarget;
  pipeline.sample_count = (colorTarget ? colorTarget->multisampled : depthTarget->multisampled) ? 4 : 1;
  require(!colorTarget || !depthTarget || colorTarget->multisampled == depthTarget->multisampled, "menu backend draw targets disagree on multisampling");
  for (uint32_t n = 0; n < 4; ++n) { if (texinfo[n].kind == gpu::kTexCube) { pipeline.cube_mask |= 1u << n; } if (texinfo[n].shadow) { pipeline.shadow_mask |= 1u << n; } if (texinfo[n].kind == gpu::kTex3D) { pipeline.volume_mask |= 1u << n; } }
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
    const uint32_t source = state.simple[gpu::kMSrcBlend / 4], destination = state.simple[gpu::kMDestBlend / 4];
    const bool colorConstant = source == 0x8001 || source == 0x8002 || destination == 0x8001 || destination == 0x8002;
    const bool alphaConstant = source == 0x8003 || source == 0x8004 || destination == 0x8003 || destination == 0x8004;
    require(!colorConstant || !alphaConstant, "menu backend unsupported mixed color and alpha blend constants");
    const uint32_t constant = state.simple[gpu::kMBlendColor / 4];
    packet.blend_constant[3] = float(constant >> 24) / 255;
    packet.blend_constant[0] = alphaConstant ? packet.blend_constant[3] : float((constant >> 16) & 255) / 255;
    packet.blend_constant[1] = alphaConstant ? packet.blend_constant[3] : float((constant >> 8) & 255) / 255;
    packet.blend_constant[2] = alphaConstant ? packet.blend_constant[3] : float(constant & 255) / 255;
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
  append(key, pipeline);
  if (raw) { append(key, rawLayout); }
  append(key, impl_->shaderSerial);
  auto foundPipeline = impl_->pipelines.find(key);
  require(budget.maxPipelines && impl_->pipelineSequence != UINT64_MAX, "menu backend pipeline cache budget exceeded");
  if (foundPipeline == impl_->pipelines.end()) {
    if (impl_->pipelines.size() >= budget.maxPipelines) {
      const auto oldest = std::min_element(impl_->pipelines.begin(), impl_->pipelines.end(), [](const auto& left, const auto& right) {
        return left.second.used < right.second.used;
      });
      impl_->send(webgpu::Opcode::Destroy, oldest->second.id);
      impl_->pipelines.erase(oldest);
    }
    std::vector<uint8_t> bytes;
    append(bytes, pipeline); if (raw) { append(bytes, rawLayout); } bytes.insert(bytes.end(), shaders.vertex.begin(), shaders.vertex.end()); bytes.insert(bytes.end(), shaders.fragment.begin(), shaders.fragment.end());
    const uint32_t id = impl_->newId();
    try { impl_->send(webgpu::Opcode::CreatePipeline, id, bytes); }
    catch (const std::exception& error) { throw std::runtime_error(menu::DescribeShaderFailure(shaders, error.what())); }
    foundPipeline = impl_->pipelines.emplace(std::move(key), Impl::Pipeline{id, 0}).first;
  }
  foundPipeline->second.used = ++impl_->pipelineSequence;
  packet.pipeline = foundPipeline->second.id;
  auto constants = menu::BuildConstants(state, target.desc.width, target.desc.height, texsize);
  if (depthBias) {
    const float offset = std::bit_cast<float>(state.simple[gpu::kMDepthOffset / 4]), slope = std::bit_cast<float>(state.simple[gpu::kMSlopeScale / 4]);
    require(std::isfinite(offset) && std::isfinite(slope) && std::isfinite(state.zscale) && state.zscale > 0, "menu backend invalid depth bias state");
    const double units = std::round(double(offset) * 16777215.0 / state.zscale);
    require(units >= INT32_MIN && units <= INT32_MAX, "menu backend depth bias exceeds signed D24 units");
    // the slope is per console pixel: a scaled target's pixels are smaller
    constants.depth_bias = {float(units / 16777215.0), slope * float(target.hostWidth) / float(target.desc.width), 0, 0};
  }
  for (uint32_t n = 0; n < 4; ++n) {
    if (!state.textures[n] || (packet.sampler[n][6] == 0 && packet.sampler[n][2] == packet.sampler[n][3])) { constants.lod_bias[n] = 0; }
  }
  // native's FillHdrConstants: colour lands over the headroom unless the blend multiplies it with what the target holds
  if (colorTarget && colorTarget->inHdr) {
    const uint32_t source = pipeline.blend ? state.simple[gpu::kMSrcBlend / 4] : 1;
    constants.hdr[0] = source != 0 && source != 0x306 ? 1 / Impl::kHeadroom : 1;
  }
  packet.uniform_bytes = sizeof(constants);
  require(uint64_t(state.viewport.x) + state.viewport.w <= target.desc.width && uint64_t(state.viewport.y) + state.viewport.h <= target.desc.height && state.viewport.w && state.viewport.h, "menu backend unsupported viewport outside target");
  require(std::isfinite(state.viewport.min_z) && std::isfinite(state.viewport.max_z) && state.viewport.min_z >= 0 && state.viewport.max_z <= 1 && state.viewport.min_z <= state.viewport.max_z, "menu backend unsupported viewport depth range");
  packet.viewport[2] = float(target.hostWidth); packet.viewport[3] = float(target.hostHeight);
  packet.viewport[5] = 1;
  uint32_t left = state.viewport.x, top = state.viewport.y;
  uint32_t right = left + state.viewport.w, bottom = top + state.viewport.h;
  if (state.scissor_count) {
    require(state.scissors[0] <= state.scissors[2] && state.scissors[1] <= state.scissors[3] && state.scissors[2] <= target.desc.width && state.scissors[3] <= target.desc.height, "menu backend unsupported scissor outside target");
    left = std::max(left, state.scissors[0]); top = std::max(top, state.scissors[1]);
    right = std::min(right, state.scissors[2]); bottom = std::min(bottom, state.scissors[3]);
  }
  const auto scissor = impl_->scaledRect(target, {int32_t(left), int32_t(top), int32_t(std::max(right, left)), int32_t(std::max(bottom, top))});
  packet.scissor[0] = uint32_t(scissor[0]); packet.scissor[1] = uint32_t(scissor[1]);
  packet.scissor[2] = uint32_t(scissor[2] - scissor[0]); packet.scissor[3] = uint32_t(scissor[3] - scissor[1]);
  if (raw) {
    webgpu::ResidentDraw draw{};
    for (uint32_t n = 0; n < 16; ++n) {
      if (!rawLayout.strides[n]) { continue; }
      const uint32_t offset = minimum * rawLayout.strides[n];
      const uint32_t size = streamBytes[n] - offset;
      const auto view = impl_->residentBuffer({0, size, 0}, 0, size, nullptr, input.streams[n].vb, offset);
      draw.vertices[n] = view.id; draw.offsets[n] = view.offset;
      draw.sizes[n] = view.bytes;
    }
    std::vector<uint32_t> indexKey{0, count, prim};
    if (indices) {
      const uint32_t origin = minimum - input.base_vertex;
      const auto version = indexSource ? impl_->memory->PhysicalWriteVersion({}, indexSource, size_t(count) * 2) : std::nullopt;
      if (version) { indexKey = {1, count, prim, indexSource, origin, uint32_t(*version), uint32_t(*version >> 32)}; }
      else {
        indexKey.resize(3 + (count + 1) / 2, 0);
        for (uint32_t n = 0; n < count; ++n) { indexKey[3 + n / 2] |= uint32_t(indexValues[n] - origin) << ((n & 1) * 16); }
      }
    }
    const auto indexView = impl_->residentBuffer(std::move(indexKey), 0, count * 2, [&vertexNumbers, prim, count, minimum] {
      const auto order = orderOf(prim, count);
      std::vector<uint8_t> expanded; expanded.reserve(order.size() * 4);
      for (const auto n : order) { append(expanded, vertexNumbers[n] - minimum); }
      return expanded;
    });
    draw.indices = indexView.id; draw.index_offset = indexView.offset;
    draw.index_bytes = uint32_t(expandedCount * 4);
    packet.vertex_count = maximum - minimum + 1; packet.index_count = uint32_t(expandedCount); packet.index32 = 2;
    std::vector<uint8_t> bytes;
    append(bytes, packet); append(bytes, draw); append(bytes, constants);
    const auto command = impl_->draw(colorTarget ? colorTarget->id : 0, packet, bytes);
    if (raw && budget.batchDraws) { impl_->stagedBufferViews.insert(impl_->viewPins.begin(), impl_->viewPins.end()); }
    if (colorTarget) { colorTarget->dirty = true; if (command) { colorTarget->command = *command; } ++colorTarget->version; }
    if (packet.depth_target) {
      auto& depth = impl_->resources.at(uint64_t(address(state.zs.data)) * 2 + 1);
      depth.dirty = true; if (command) { depth.command = *command; } ++depth.version;
    }
    impl_->bufferPins.clear(); impl_->viewPins.clear();
    return;
  }
  const auto order = orderOf(prim, count);
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
    const auto& resource = captured.at(requestAt(stream.vb).value());
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
    const size_t pitch = size_t(vertexRegisters) * 4;
    for (uint32_t n = 0; n < sourceCount; ++n) {
      float* column = vertices.data() + n * 4;
      for (size_t vertex = 0; vertex < batchNumbers.size(); ++vertex) { column[vertex * pitch + 3] = 1; }
      const auto& source = sources[n];
      if (source.type) { convertColumn(source.type, source.first, source.stride, batchNumbers, column, pitch); }
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
  impl_->feedbackPins.clear();
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
  if (target && target->inHdr) { for (uint32_t n = 0; n < 3; ++n) { packet.color[n] /= Impl::kHeadroom; } }
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
    append(bytes, request);
    const auto& sized = colorTarget ? *colorTarget : *depthTarget;
    for (size_t n = 0; n + 16 <= rectangles.size(); n += 16) {
      std::array<int32_t, 4> rect;
      std::memcpy(rect.data(), rectangles.data() + n, 16);
      rect = impl_->scaledRect(sized, rect);
      appendBytes(bytes, {reinterpret_cast<const uint8_t*>(rect.data()), 16});
    }
    const auto result = impl_->send(webgpu::Opcode::Clear, colorTarget ? colorTarget->id : 0, bytes);
    for (auto* item : {colorTarget, depthTarget}) {
      if (!item) { continue; }
      item->depthInitialized |= item == depthTarget && (clearFlags & 2) != 0;
      item->dirty = true; item->command = result.command; ++item->version;
    }
  };
  if (target && depth && (target->hostWidth != depth->hostWidth || target->hostHeight != depth->hostHeight)) {
    submit(target, nullptr, 1); submit(nullptr, depth, packet.flags & 6);
  } else { submit(target, depth, packet.flags); }
}
void MenuBackend::Copy(const gpu::Surface& source, const gpu::Surface& destination, uint32_t count, uint32_t sourceRects, uint32_t destinationPoints) {
  std::lock_guard lock(impl_->mutex);
  impl_->feedbackPins.clear();
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
  impl_->resolve(from);
  const bool sameFormat = from.hdrTarget == to.hdrTarget && !to.inHdr;
  const float scale = to.inHdr ? 1 / Impl::kHeadroom : 1;
  const auto colorFamily = [](uint32_t format) { return format == 0x06 || format == 0x12 ? 1u : format == 0x07 || format == 0x1e ? 2u : 0u; };
  require(colorFamily(from.desc.format) && colorFamily(to.desc.format), "menu backend incompatible surface copy");
  const auto copies = regions(1);
  if (copies.empty()) { return; }
  const uint32_t sourceId = from.id == to.id ? impl_->feedbackTexture(from) : from.id;
  const float fx = float(from.hostWidth) / from.desc.width, fy = float(from.hostHeight) / from.desc.height;
  const float tx = float(to.hostWidth) / to.desc.width, ty = float(to.hostHeight) / to.desc.height;
  for (auto packet : copies) {
    packet.source = sourceId;
    packet.force_opaque = colorFamily(to.desc.format) == 2;
    webgpu::Result result;
    if (fx == tx && fy == ty && sameFormat) {
      const auto source = impl_->scaledRect(from, {int32_t(packet.source_x), int32_t(packet.source_y), int32_t(packet.source_x + packet.width), int32_t(packet.source_y + packet.height)});
      const auto destination = impl_->scaledRect(to, {int32_t(packet.destination_x), int32_t(packet.destination_y), int32_t(packet.destination_x + packet.width), int32_t(packet.destination_y + packet.height)});
      packet.source_x = uint32_t(source[0]); packet.source_y = uint32_t(source[1]);
      packet.destination_x = uint32_t(destination[0]); packet.destination_y = uint32_t(destination[1]);
      packet.width = uint32_t(std::min(source[2] - source[0], destination[2] - destination[0]));
      packet.height = uint32_t(std::min(source[3] - source[1], destination[3] - destination[1]));
      if (!packet.width || !packet.height) { continue; }
      result = impl_->sendPacket(webgpu::Opcode::Copy, to.id, packet);
    } else {
      result = impl_->blit(sourceId, to.id, {packet.source_x * fx, packet.source_y * fy, packet.width * fx, packet.height * fy},
        {packet.destination_x * tx, packet.destination_y * ty, packet.width * tx, packet.height * ty}, packet.force_opaque, scale);
    }
    to.dirty = true; to.command = result.command; ++to.version;
  }
}
bool MenuBackend::HasGpuContents(uint32_t location) const {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  const auto found = impl_->resources.find(uint64_t(address(location)) * 2);
  return found != impl_->resources.end() && found->second.renderTarget && found->second.dirty;
}
void MenuBackend::SetOutputSource(std::function<Output()> source) {
  std::lock_guard lock(impl_->mutex);
  impl_->outputSource = std::move(source);
}
void MenuBackend::Present(const gpu::Surface& front, bool pillarbox) {
  std::lock_guard lock(impl_->mutex);
  impl_->feedbackPins.clear();
  require(impl_->initialized, "menu backend present before initialization");
  auto& item = impl_->surface(front);
  const Output output = impl_->outputSource ? impl_->outputSource() : Output{};
  impl_->tone = output.toneMapping;
  impl_->resolve(item);
  if (output.scale > 0) { impl_->applyScale(output.scale); }
  impl_->anisotropy = output.anisotropy;
  impl_->smaa = output.smaa;
  if (const uint32_t edge = std::clamp(output.shadowResolution, 512u, 4096u); edge != impl_->shadowResolution) {
    impl_->shadowResolution = edge;
    for (auto& [key, resource] : impl_->resources) {
      if (!resource.renderTarget) { continue; }
      const auto [w, h] = impl_->hostSize(resource.desc, true);
      impl_->resample(resource, w, h);
    }
  }
  if (output.brightness != impl_->brightness || output.contrast != impl_->contrast) {
    impl_->brightness = output.brightness; impl_->contrast = output.contrast;
    impl_->sendGamma();
  }
  if (output.width && output.height && (output.width != impl_->canvasWidth || output.height != impl_->canvasHeight)) {
    require(output.width <= 8192 && output.height <= 8192, "menu backend canvas dimension budget exceeded");
    impl_->sendPacket(webgpu::Opcode::Configure, 0, webgpu::Dimensions{output.width, output.height});
    impl_->canvasWidth = output.width;
    impl_->canvasHeight = output.height;
  }
  const uint32_t flags = (pillarbox ? webgpu::PresentPillarbox : 0) | (output.linear ? webgpu::PresentLinear : 0) | (output.smaa ? webgpu::PresentSmaa : 0) |
    (output.hdrOutput ? webgpu::PresentHdr : 0);
  const auto result = impl_->sendPacket(webgpu::Opcode::Present, item.id, webgpu::PresentPacket{flags, std::clamp(output.hdrHeadroom, 1.0f, 10.0f)});
  item.command = result.command; ++impl_->frames;
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
  impl_->collectLate(item);
  if (impl_->startLate(item)) { item.lateVersion = item.version; }
}
bool MenuBackend::DeliverLate(uint32_t location) {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  auto& item = impl_->byAddress(location);
  impl_->collectLate(item);
  if (item.late.empty()) { return false; }
  require(impl_->readHeader(item), "menu backend stale late readback resource header");
  impl_->publish(item, item.late, 0, uint32_t(item.late.size()));
  impl_->resident -= item.late.size();
  item.dirty = item.version != item.lateVersion;
  item.delivered = std::move(item.late);
  item.late = std::vector<uint8_t>{};
  return true;
}
void MenuBackend::SyncLate(uint32_t location) {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->requireOwner();
    auto& item = impl_->byAddress(location);
    if (item.lateStarted && !item.delivered.empty() && !impl_->collectLate(item, true)) {
      require(impl_->readHeader(item), "menu backend stale late readback resource header");
      impl_->publish(item, item.delivered, 0, uint32_t(item.delivered.size()));
      return;
    }
  }
  if (!DeliverLate(location)) { Sync(location, 0); }
  RecordLate(location);
}
void MenuBackend::Flush() { std::lock_guard lock(impl_->mutex); impl_->send(webgpu::Opcode::Flush, 0); }
void MenuBackend::SetGamma(const uint8_t* ramp) {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  require(ramp, "menu backend missing gamma ramp");
  std::memcpy(impl_->guestRamp.data(), ramp, impl_->guestRamp.size());
  impl_->sendGamma();
}
void MenuBackend::Occlusion(const gpu::State&, const float*) { throw std::runtime_error("menu backend unsupported occlusion effect"); }
// native FilterShadows: the frame's own scene only, its depth at its size and samples, a non-empty viewport; the passes
// grow with the integer render scale
void MenuBackend::FilterShadows(const gpu::State& state, const float* p) {
  std::lock_guard lock(impl_->mutex);
  impl_->softScene = 0;
  if (!state.rt.data || !state.zs.data) { return; }
  const auto colour = impl_->resources.find(uint64_t(address(state.rt.data)) * 2);
  const auto depth = impl_->resources.find(uint64_t(address(state.zs.data)) * 2 + 1);
  if (colour == impl_->resources.end() || depth == impl_->resources.end()) { return; }
  auto& rt = colour->second; auto& ds = depth->second;
  if (rt.desc.width != impl_->width || rt.desc.height != impl_->height || ds.hostWidth != rt.hostWidth || ds.hostHeight != rt.hostHeight ||
      ds.multisampled != rt.multisampled) { return; }
  const auto& v = state.viewport;
  const float zr = v.max_z - v.min_z;
  const auto rect = impl_->scaledRect(rt, {int32_t(v.x), int32_t(v.y), int32_t(v.x + v.w), int32_t(v.y + v.h)});
  const int32_t left = std::clamp(rect[0], 0, int32_t(rt.hostWidth)), top = std::clamp(rect[1], 0, int32_t(rt.hostHeight));
  const int32_t right = std::clamp(rect[2], 0, int32_t(rt.hostWidth)), bottom = std::clamp(rect[3], 0, int32_t(rt.hostHeight));
  if (right <= left || bottom <= top || zr <= 0) { return; }
  const uint32_t scale = std::max(1u, uint32_t(std::lround(float(rt.hostHeight) / float(impl_->height))));
  webgpu::SoftShadowPacket packet{ds.id, {p[0] * zr, v.min_z + p[1] * zr, 0, 0}, {float(left), float(top), float(right - left), float(bottom - top)},
    {float(256 * scale), float(128 * scale), 0, 0}};
  impl_->initializeDepth(ds);
  impl_->sendPacket(webgpu::Opcode::SoftShadows, rt.id, packet);
  impl_->softScene = rt.desc.data;
}
void MenuBackend::ApplyShadows(const gpu::State& state) {
  std::lock_guard lock(impl_->mutex);
  const bool pending = impl_->softScene && state.rt.data && address(state.rt.data) == impl_->softScene;
  impl_->softScene = 0;
  if (!pending) { return; }
  auto& rt = impl_->resources.at(uint64_t(address(state.rt.data)) * 2);
  const auto result = impl_->send(webgpu::Opcode::ApplyShadows, rt.id);
  rt.dirty = true; rt.command = result.command; ++rt.version;
}
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
  require(!impl_->releasedResources.contains(resource), "menu backend unknown resource");
  const uint32_t location = address(resource);
  if (std::any_of(impl_->resources.begin(), impl_->resources.end(), [&](const auto& entry) {
    return location >= entry.second.desc.data && uint64_t(location) < uint64_t(entry.second.desc.data) + entry.second.uploaded.size();
  })) { return impl_->byAddress(resource).command > impl_->completed; }
  const auto header = impl_->read(resource, 12);
  require((word(header, 0) & 0x70000) == 0 && (word(header, 0) & 0x78ffff) && word(header, 4), "menu backend unknown CPU upload resource");
  return false;
}
void MenuBackend::WaitResource(uint32_t resource) { if (Busy(resource)) { Flush(); } }
bool MenuBackend::ReleaseResource(uint32_t header, uint32_t data) {
  std::lock_guard lock(impl_->mutex);
  impl_->requireOwner();
  impl_->uploadVersions.erase(header);
  if (header && impl_->limits.maxResources) {
    impl_->releasedResources.insert(header);
    if (impl_->releasedResources.size() > impl_->limits.maxResources) { impl_->releasedResources.erase(impl_->releasedResources.begin()); }
  }
  auto found = std::find_if(impl_->resources.begin(), impl_->resources.end(), [&](const auto& entry) {
    return entry.second.desc.data == address(data) && entry.second.views.contains(header);
  });
  if (found == impl_->resources.end()) { return false; }
  auto& item = found->second;
  if (item.views.size() > 1) { item.views.erase(header); return false; }
  impl_->collectLate(item);
  if (item.dirty) { impl_->publish(item, impl_->readback(item), 0, uint32_t(item.uploaded.size())); }
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
