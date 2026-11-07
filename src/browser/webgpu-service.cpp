#include "gpu/smaa_textures.h"
#include "gpu/smaa_wgsl.h"
#include "webgpu-transport.h"
#include <webgpu/webgpu.h>
#include <emscripten.h>
#include <array>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace browser::webgpu;
namespace {
WGPUInstance instance;
WGPUAdapter adapter;
WGPUDevice device;
WGPUQueue queue;
WGPUCommandEncoder batch_encoder;
WGPURenderPassEncoder batch_pass;
uint32_t batch_color = 0, batch_depth = 0;
void end_batch_pass() {
  if (batch_pass) { wgpuRenderPassEncoderEnd(batch_pass); wgpuRenderPassEncoderRelease(batch_pass); batch_pass = nullptr; }
}
WGPUSurface surface;
WGPUBindGroupLayout bindings;
WGPUPipelineLayout layout;
WGPUSampler sampler;
struct Texture {
  WGPUTexture handle;
  uint32_t width, height;
  bool depth;
  uint32_t levels = 1;
  bool cube = false;
  WGPUTextureView sampled = nullptr;
  WGPUTextureView attachment = nullptr;
  uint32_t volumeDepth = 1;
  // MSAA: a 4x companion the draws render into; colour resolves into the texture at every pass's end, depth keeps
  // only the companion current (stale) until a read resolves its sample 0. seed: the texture is newer than the
  // companion, which takes its texels before the next pass draws into it.
  WGPUTexture ms = nullptr;
  WGPUTextureView msView = nullptr;
  bool seed = false;
  bool stale = false;
  bool hdr = false;  // an HDR scene target in hdr_format
};
// HDR scene targets: RGBA16Float. Native's RGBA16Unorm comes with texture-formats-tier1, but only unfilterable, and the
// game samples its scene through filtering samplers
constexpr WGPUTextureFormat hdr_format = WGPUTextureFormat_RGBA16Float;
WGPUTextureFormat color_format(const Texture& texture) { return texture.hdr ? hdr_format : WGPUTextureFormat_RGBA8Unorm; }
uint32_t texel_bytes(const Texture& texture) { return texture.hdr ? 8 : 4; }
struct ResidentBuffer { WGPUBuffer handle; uint32_t bytes; };
std::map<uint32_t, ResidentBuffer> resident_buffers;
uint64_t resident_buffer_bytes = 0;
struct CachedSampler { WGPUSampler handle; uint64_t used; };
std::map<std::array<uint32_t, 9>, CachedSampler> sampler_cache;
uint64_t sampler_sequence = 0;
uint32_t cached_views = 0;
WGPUSampler cached_sampler(const uint32_t (&state)[7], uint32_t addressW, uint32_t anisotropy) {
  std::array<uint32_t, 9> key;
  std::copy(std::begin(state), std::end(state), key.begin());
  key[7] = addressW; key[8] = anisotropy;
  auto found = sampler_cache.find(key);
  if (found == sampler_cache.end()) {
    if (sampler_cache.size() == 128) {
      const auto oldest = std::min_element(sampler_cache.begin(), sampler_cache.end(), [](const auto& left, const auto& right) {
        return left.second.used < right.second.used;
      });
      wgpuSamplerRelease(oldest->second.handle);
      sampler_cache.erase(oldest);
    }
    auto descriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
    descriptor.addressModeW = static_cast<WGPUAddressMode>(addressW);
    descriptor.addressModeU = static_cast<WGPUAddressMode>(state[0]); descriptor.addressModeV = static_cast<WGPUAddressMode>(state[1]);
    descriptor.magFilter = static_cast<WGPUFilterMode>(state[2]); descriptor.minFilter = static_cast<WGPUFilterMode>(state[3]);
    descriptor.mipmapFilter = static_cast<WGPUMipmapFilterMode>(state[4]); descriptor.lodMinClamp = float(state[5]); descriptor.lodMaxClamp = float(state[6]);
    descriptor.maxAnisotropy = uint16_t(anisotropy);
    found = sampler_cache.emplace(key, CachedSampler{wgpuDeviceCreateSampler(device, &descriptor), 0}).first;
  }
  found->second.used = ++sampler_sequence;
  wgpuSamplerAddRef(found->second.handle);
  return found->second.handle;
}
WGPUTextureView sampled_view(Texture& texture) {
  if (!texture.sampled) {
    auto descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    if (texture.depth) { descriptor.aspect = WGPUTextureAspect_DepthOnly; }
    descriptor.dimension = texture.cube ? WGPUTextureViewDimension_Cube : texture.volumeDepth > 1 ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
    texture.sampled = wgpuTextureCreateView(texture.handle, &descriptor);
    ++cached_views;
  }
  return texture.sampled;
}
WGPUTextureView attachment_view(Texture& texture) {
  if (!texture.attachment) { texture.attachment = wgpuTextureCreateView(texture.handle, nullptr); ++cached_views; }
  return texture.attachment;
}
void release_views(Texture& texture) {
  if (texture.sampled) { wgpuTextureViewRelease(texture.sampled); texture.sampled = nullptr; --cached_views; }
  if (texture.attachment) { wgpuTextureViewRelease(texture.attachment); texture.attachment = nullptr; --cached_views; }
  if (texture.msView) { wgpuTextureViewRelease(texture.msView); texture.msView = nullptr; }
  if (texture.ms) { wgpuTextureDestroy(texture.ms); wgpuTextureRelease(texture.ms); texture.ms = nullptr; }
}
uint64_t textureBytes(uint32_t width, uint32_t height, uint32_t levels, uint32_t depth = 1, uint32_t texel = 4) {
  uint64_t bytes = 0;
  for (uint32_t n = 0; n < levels; ++n) { bytes += uint64_t(std::max(width >> n, 1u)) * std::max(height >> n, 1u) * std::max(depth >> n, 1u) * texel; }
  return bytes;
}
uint64_t multisample_bytes(const Texture& texture) { return uint64_t(texture.width) * texture.height * 4 * texel_bytes(texture); }
std::map<uint32_t, Texture> textures;
void readable(Texture& texture);
void seed(WGPUCommandEncoder encoder, Texture& texture);
struct Pipeline { WGPURenderPipeline handle; uint32_t vertexStride; };
std::map<uint32_t, Pipeline> pipelines;
std::vector<uint8_t> reply;
std::string failure;
struct AsyncRead { bool done = false; std::vector<uint8_t> bytes; std::string failure; };
std::map<uint32_t, AsyncRead> async_reads;
std::array<AsyncRead, ReadbackSlots> slot_reads;
uint32_t collect_waiting = 0;
uint32_t presents_in_flight = 0;
bool present_waiting = false;
bool closed = false;
bool active = false;
uint32_t pending_validations = 0;
bool validation_barrier = false;
uint32_t created_resource = 0;
uint64_t texture_bytes = 0;
// screen sized targets at 1080p with 4x MSAA and HDR are about 66 MB a colour companion, so the console era 256 MiB is too small
constexpr uint64_t TextureBudget = 1024ull * 1024 * 1024;
std::runtime_error budget_exhausted(const char* what, uint64_t needed) {
  return std::runtime_error(std::string("GPU ") + what + " texture budget exhausted: " + std::to_string(texture_bytes >> 20) + " MiB used, " + std::to_string(needed >> 20) + " MiB more");
}
uint32_t canvas_width = 0, canvas_height = 0;
WGPUTexture gamma_texture;
WGPURenderPipeline presentation_pipeline;
WGPURenderPipeline presentation_linear_pipeline;
WGPUBindGroupLayout presentation_bindings;
WGPUPipelineLayout presentation_layout;
Texture presentation_target{};
WGPURenderPipeline depth_upload_pipeline;
WGPUBindGroupLayout depth_upload_bindings;
WGPUPipelineLayout depth_upload_layout;
WGPURenderPipeline depth_readback_pipeline;
WGPUBindGroupLayout depth_readback_bindings;
WGPUPipelineLayout depth_readback_layout;
std::array<WGPURenderPipeline, 36> partial_clear_pipelines{};  // keys 9 to 17: the same for 4x companions, 18 on: HDR
WGPUBindGroupLayout partial_clear_bindings;
WGPUPipelineLayout partial_clear_layout;
std::array<uint8_t, 768> gamma_ramp = [] {
  std::array<uint8_t, 768> result{};
  for (uint32_t n = 0; n < result.size(); ++n) { result[n] = n % 256; }
  return result;
}();
void discard(uint32_t id) {
  async_reads.erase(id);
  if (collect_waiting == id) { collect_waiting = 0; }
  const auto buffer = resident_buffers.find(id);
  if (buffer != resident_buffers.end()) { resident_buffer_bytes -= buffer->second.bytes; wgpuBufferDestroy(buffer->second.handle); wgpuBufferRelease(buffer->second.handle); resident_buffers.erase(buffer); }
  auto image = textures.find(id);
  if (image != textures.end()) { texture_bytes -= textureBytes(image->second.width, image->second.height, image->second.levels, image->second.volumeDepth, texel_bytes(image->second)) * (image->second.cube ? 6 : 1) + (image->second.ms ? multisample_bytes(image->second) : 0); release_views(image->second); wgpuTextureDestroy(image->second.handle); wgpuTextureRelease(image->second.handle); textures.erase(image); }
  auto pipeline = pipelines.find(id);
  if (pipeline != pipelines.end()) { wgpuRenderPipelineRelease(pipeline->second.handle); pipelines.erase(pipeline); }
}
WGPUStringView text(const char* value) { return {value, std::strlen(value)}; }
EM_JS(void, ready, (int status, const char* message), { Module.onGPUReady(status, UTF8ToString(message)); });
EM_JS(void, adapter_details, (const char* vendor, const char* architecture, const char* description, const uint32_t* features, uint32_t count, uint32_t max_texture, uint32_t max_attributes, uint32_t max_stride, uint32_t max_uniform, uint32_t compression), {
  Module.onGPUAdapter?.({ vendor: UTF8ToString(vendor), architecture: UTF8ToString(architecture), description: UTF8ToString(description), features: Array.from(HEAPU32.subarray(features >> 2, (features >> 2) + count)), textureCompressionBC: Boolean(compression & 1), textureCompressionETC2: Boolean(compression & 2), textureCompressionASTC: Boolean(compression & 4), limits: { maxTextureDimension2D: max_texture, maxVertexAttributes: max_attributes, maxVertexBufferArrayStride: max_stride, maxUniformBufferBindingSize: max_uniform }, source: 'service-adapter' });
});
EM_JS(void, completed, (int status, const uint8_t* bytes, uint32_t size, const char* message), { Module.onGPUComplete(status, HEAPU8.slice(bytes, bytes + size), UTF8ToString(message)); });
EM_JS(void, uncaptured, (const char* message), { Module.onGPUError(UTF8ToString(message)); });
EM_JS(void, lost, (const char* message), { Module.onGPULost(UTF8ToString(message)); });
EM_JS(void, deferred_failure, (uint32_t command, uint32_t resource, const char* message), {
  Module.onGPUDeferredError?.({ command, resource, status: 1, message: UTF8ToString(message) });
});
EM_JS(void, deliver_readback, (uint32_t slot, uint32_t resource, int status, const uint8_t* bytes, uint32_t size), {
  const mailbox = Module.readbackMailbox;
  if (!mailbox) { Module.printErr('asynchronous readback mailbox unavailable'); return; }
  const base = 64 + 4194304 + slot * (64 + 4194304);
  if (slot >= 4 || mailbox.byteLength < base + 64 + 4194304 || size > 4194304) { Module.printErr('asynchronous readback slot exceeds mailbox ' + mailbox.byteLength); return; }
  const header = new Int32Array(mailbox, base, 4);
  new Uint8Array(mailbox, base + 64, size).set(HEAPU8.subarray(bytes, bytes + size));
  header[1] = resource; header[2] = size; header[3] = status;
  Atomics.store(header, 0, 2);
  Atomics.notify(header, 0);
});
EM_JS(void, register_canvas, (), { specialHTMLTargets['#canvas'] = Module.canvas; });
EM_JS(void, resize_canvas, (uint32_t width, uint32_t height), { Module.canvas.width = width; Module.canvas.height = height; });
std::string string(WGPUStringView value) { return std::string(value.data ? value.data : "", value.data ? value.length : 0); }
void complete_active() {
  if (!active || closed) { return; }
  active = false;
  if (!failure.empty() && created_resource) { discard(created_resource); }
  created_resource = 0;
  completed(failure.empty() ? 0 : 1, reply.data(), reply.size(), failure.c_str());
}
void finish() {
  auto info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
  info.mode = WGPUCallbackMode_AllowSpontaneous;
  info.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void*, void*) {
    if (type != WGPUErrorType_NoError || status != WGPUPopErrorScopeStatus_Success) { failure = string(message); }
    if (!active) { return; }
    if (failure.empty() && pending_validations) { validation_barrier = true; return; }
    complete_active();
  };
  wgpuDevicePopErrorScope(device, info);
}
void defer_validation(uint32_t command, uint32_t resource) {
  struct Identity { uint32_t command, resource; };
  auto* identity = new Identity{command, resource};
  ++pending_validations;
  auto info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
  info.mode = WGPUCallbackMode_AllowSpontaneous;
  info.userdata1 = identity;
  info.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* data, void*) {
    const auto identity = *static_cast<Identity*>(data);
    delete static_cast<Identity*>(data);
    --pending_validations;
    if (closed) { return; }
    if (type != WGPUErrorType_NoError || status != WGPUPopErrorScopeStatus_Success) {
      closed = true; active = false; validation_barrier = false;
      const auto detail = string(message);
      deferred_failure(identity.command, identity.resource, detail.empty() ? "deferred GPU validation failed" : detail.c_str());
      return;
    }
    if (!pending_validations && validation_barrier) { validation_barrier = false; complete_active(); }
  };
  active = false;
  completed(0, nullptr, 0, "");
  wgpuDevicePopErrorScope(device, info);
}
Texture& texture(uint32_t id) {
  auto found = textures.find(id);
  if (found == textures.end()) { throw std::runtime_error("unknown texture resource"); }
  return found->second;
}
template<class T> T packet(std::span<const uint8_t>& bytes) {
  if (bytes.size() < sizeof(T)) { throw std::runtime_error("truncated GPU packet"); }
  T result;
  std::memcpy(&result, bytes.data(), sizeof(T));
  bytes = bytes.subspan(sizeof(T));
  return result;
}
void exact(std::span<const uint8_t> bytes, uint64_t size) {
  if (bytes.size() != size) { throw std::runtime_error("GPU payload length mismatch"); }
}
// per frame GPU work, kept for the presented frame's diagnostics: render passes, those resolving a 4x companion,
// companion seeds, depth sample 0 resolves, queue submits, texture MiB, resident buffer KiB uploaded
enum FrameStat : uint32_t { StatPasses, StatResolvingPasses, StatSeeds, StatDepthResolves, StatSubmits, StatTextureMiB, StatUploadKiB, StatCount };
uint64_t frame_upload_bytes = 0;
std::array<uint32_t, StatCount> frame_stats{}, presented_stats{};
WGPURenderPassEncoder begin_pass(WGPUCommandEncoder encoder, const WGPURenderPassDescriptor* descriptor) {
  ++frame_stats[StatPasses];
  if (descriptor->colorAttachmentCount && descriptor->colorAttachments[0].resolveTarget) { ++frame_stats[StatResolvingPasses]; }
  return wgpuCommandEncoderBeginRenderPass(encoder, descriptor);
}
void submit(WGPUCommandEncoder encoder) {
  auto command = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(queue, 1, &command);
  ++frame_stats[StatSubmits];
  wgpuCommandBufferRelease(command);
  wgpuCommandEncoderRelease(encoder);
}
void write_buffer(WGPUBuffer buffer, std::span<const uint8_t> bytes, uint64_t offset = 0) {
  if (bytes.size() % 4 == 0) { wgpuQueueWriteBuffer(queue, buffer, offset, bytes.data(), bytes.size()); return; }
  std::vector<uint8_t> padded((bytes.size() + 3) & ~size_t(3));
  std::memcpy(padded.data(), bytes.data(), bytes.size());
  wgpuQueueWriteBuffer(queue, buffer, offset, padded.data(), padded.size());
}
WGPUBuffer buffer(std::span<const uint8_t> bytes, WGPUBufferUsage usage) {
  auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
  descriptor.size = (bytes.size() + 3) & ~uint64_t(3);
  descriptor.usage = usage | WGPUBufferUsage_CopyDst;
  auto result = wgpuDeviceCreateBuffer(device, &descriptor);
  write_buffer(result, bytes);
  return result;
}
void upload_buffer(uint32_t resource, std::span<const uint8_t> bytes) {
  const auto upload = packet<BufferUpload>(bytes);
  const uint64_t length = (bytes.size() + 3) & ~uint64_t(3);
  frame_upload_bytes += bytes.size();
  if (!resource || bytes.empty() || upload.offset % 4 || !upload.capacity || upload.capacity % 4 || upload.capacity > MaxBytes || uint64_t(upload.offset) + length > upload.capacity) { throw std::runtime_error("invalid resident buffer upload"); }
  if (const auto found = resident_buffers.find(resource); found != resident_buffers.end()) {
    if (upload.capacity != found->second.bytes) { throw std::runtime_error("resident buffer update size mismatch"); }
    write_buffer(found->second.handle, bytes, upload.offset); return;
  }
  if (textures.contains(resource) || pipelines.contains(resource) || resident_buffers.size() >= 512 || textures.size() + pipelines.size() + resident_buffers.size() >= 4096 || resident_buffer_bytes + upload.capacity > (128u << 20)) { throw std::runtime_error("resident buffer budget exceeded"); }
  auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
  descriptor.size = upload.capacity; descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
  const auto handle = wgpuDeviceCreateBuffer(device, &descriptor);
  resident_buffers.emplace(resource, ResidentBuffer{handle, upload.capacity}); resident_buffer_bytes += upload.capacity; created_resource = resource;
  write_buffer(handle, bytes, upload.offset);
  return;
}
struct DrawUploads {
  std::array<std::vector<uint8_t>, 3> bytes;
  std::array<WGPUBuffer, 3> buffers{};
  std::vector<std::array<uint64_t, 3>> offsets;
  void prepare(std::span<const uint8_t> packets, uint32_t count, bool mixed = false) {
    for (uint32_t n = 0; n < count; ++n) {
      const auto operation = mixed ? static_cast<Opcode>(packet<uint32_t>(packets)) : Opcode::Draw;
      if (operation != Opcode::Draw && operation != Opcode::Clear && operation != Opcode::Copy) { throw std::runtime_error("unsupported staged render command"); }
      packet<uint32_t>(packets);
      const uint32_t length = packet<uint32_t>(packets);
      if (length > packets.size()) { throw std::runtime_error("truncated batched draw"); }
      auto payload = packets.first(length);
      if (operation != Opcode::Draw) {
        offsets.push_back({});
        packets = packets.subspan(length);
        continue;
      }
      const auto draw = packet<DrawPacket>(payload);
      if (draw.index32 == 2) { packet<ResidentDraw>(payload); }
      exact(payload, uint64_t(draw.vertex_bytes) + draw.index_bytes + draw.uniform_bytes);
      std::array<uint64_t, 3> row{};
      const std::array<uint32_t, 3> lengths{draw.vertex_bytes, draw.index_bytes, draw.uniform_bytes};
      for (uint32_t n = 0; n < 3; ++n) {
        const uint32_t alignment = n == 2 ? 256 : 4;
        auto& arena = bytes[n];
        arena.resize((arena.size() + alignment - 1) / alignment * alignment);
        row[n] = arena.size();
        arena.insert(arena.end(), payload.begin(), payload.begin() + lengths[n]);
        payload = payload.subspan(lengths[n]);
      }
      offsets.push_back(row);
      packets = packets.subspan(length);
    }
    exact(packets, 0);
    constexpr std::array<WGPUBufferUsage, 3> usage{WGPUBufferUsage_Vertex, WGPUBufferUsage_Index, WGPUBufferUsage_Uniform};
    for (uint32_t n = 0; n < 3; ++n) {
      if (!bytes[n].empty()) { buffers[n] = buffer(bytes[n], usage[n]); }
    }
  }
  ~DrawUploads() {
    for (const auto item : buffers) { if (item) { wgpuBufferRelease(item); } }
  }
};
WGPURenderPipeline partial_clear_pipeline(uint32_t key) {
  const bool multisampled = key % 18 >= 9, hdr = key >= 18;
  const uint32_t flags = key % 9 == 8 ? 1 : key % 9;
  if (partial_clear_pipelines[key]) { return partial_clear_pipelines[key]; }
  if (!partial_clear_layout) {
    auto entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = 0; entry.visibility = WGPUShaderStage_Fragment; entry.buffer.type = WGPUBufferBindingType_Uniform;
    auto bindings = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bindings.entryCount = 1; bindings.entries = &entry;
    partial_clear_bindings = wgpuDeviceCreateBindGroupLayout(device, &bindings);
    auto layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; layout.bindGroupLayoutCount = 1; layout.bindGroupLayouts = &partial_clear_bindings;
    partial_clear_layout = wgpuDeviceCreatePipelineLayout(device, &layout);
  }
  std::string code = R"(
struct Parameters { color: vec4<f32>, depth: vec4<f32> }
@group(0) @binding(0) var<uniform> parameters: Parameters;
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
  let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(positions[index], 0.0, 1.0);
}
)";
  if (flags & 3) {
    code += "struct Result {\n";
    if (flags & 1) { code += "@location(0) color: vec4<f32>,\n"; }
    if (flags & 2) { code += "@builtin(frag_depth) depth: f32,\n"; }
    code += "}\n@fragment fn fs_main() -> Result { var result: Result;\n";
    if (flags & 1) { code += "result.color = parameters.color;\n"; }
    if (flags & 2) { code += "result.depth = parameters.depth.x;\n"; }
    code += "return result; }\n";
  } else { code += "@fragment fn fs_main() {}\n"; }
  auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = {code.data(), code.size()};
  auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
  auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
  auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = hdr ? hdr_format : WGPUTextureFormat_RGBA8Unorm;
  target.writeMask = key % 9 == 8 ? WGPUColorWriteMask_Alpha : WGPUColorWriteMask_All;
  auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text("fs_main");
  if (flags & 1) { fragment.targetCount = 1; fragment.targets = &target; }
  auto depth = WGPU_DEPTH_STENCIL_STATE_INIT; depth.format = WGPUTextureFormat_Depth24PlusStencil8;
  depth.depthWriteEnabled = flags & 2 ? WGPUOptionalBool_True : WGPUOptionalBool_False; depth.depthCompare = WGPUCompareFunction_Always;
  depth.stencilFront.compare = depth.stencilBack.compare = WGPUCompareFunction_Always;
  depth.stencilFront.passOp = depth.stencilBack.passOp = flags & 4 ? WGPUStencilOperation_Replace : WGPUStencilOperation_Keep;
  depth.stencilReadMask = 255; depth.stencilWriteMask = flags & 4 ? 255 : 0;
  auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT; descriptor.layout = partial_clear_layout;
  descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main"); descriptor.fragment = &fragment;
  descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  if (multisampled) { descriptor.multisample.count = 4; }
  if (flags & 6) { descriptor.depthStencil = &depth; }
  partial_clear_pipelines[key] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
  wgpuShaderModuleRelease(shader);
  return partial_clear_pipelines[key];
}
void force_opaque_alpha(WGPUCommandEncoder encoder, Texture& target, const CopyPacket& copy) {
  const auto pipeline = partial_clear_pipeline(target.hdr ? 26 : 8);
  const std::array<float, 8> parameters{0, 0, 0, 1};
  auto uniforms = buffer({reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)}, WGPUBufferUsage_Uniform);
  auto entry = WGPU_BIND_GROUP_ENTRY_INIT; entry.binding = 0; entry.buffer = uniforms; entry.size = sizeof(parameters);
  auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = partial_clear_bindings; group_descriptor.entryCount = 1; group_descriptor.entries = &entry;
  auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
  auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  attachment.view = wgpuTextureCreateView(target.handle, nullptr); attachment.loadOp = WGPULoadOp_Load; attachment.storeOp = WGPUStoreOp_Store;
  auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &attachment;
  auto pass = begin_pass(encoder, &descriptor);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
  wgpuRenderPassEncoderSetScissorRect(pass, copy.destination_x, copy.destination_y, copy.width, copy.height);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
  wgpuTextureViewRelease(attachment.view); wgpuBindGroupRelease(group); wgpuBufferRelease(uniforms);
}

WGPUTexture pack_depth(WGPUCommandEncoder encoder, const Texture& target) {
  if (!depth_readback_pipeline) {
    auto entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = 0; entry.visibility = WGPUShaderStage_Fragment;
    entry.texture.sampleType = WGPUTextureSampleType_Depth; entry.texture.viewDimension = WGPUTextureViewDimension_2D;
    auto group = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; group.entryCount = 1; group.entries = &entry;
    depth_readback_bindings = wgpuDeviceCreateBindGroupLayout(device, &group);
    auto layout_descriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_descriptor.bindGroupLayoutCount = 1; layout_descriptor.bindGroupLayouts = &depth_readback_bindings;
    depth_readback_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
    auto source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = text(R"(
@group(0) @binding(0) var image: texture_depth_2d;
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
  let positions = array<vec2<f32>, 3>(vec2<f32>(-1,-1), vec2<f32>(3,-1), vec2<f32>(-1,3));
  return vec4<f32>(positions[index], 0, 1);
}
@fragment fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  let depth = u32(round(clamp(textureLoad(image, vec2<i32>(position.xy), 0), 0.0, 1.0) * 16777215.0));
  return vec4<f32>(0.0, f32(depth & 255u), f32((depth >> 8u) & 255u), f32(depth >> 16u)) / 255.0;
}
)");
    auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
    auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    auto color = WGPU_COLOR_TARGET_STATE_INIT; color.format = WGPUTextureFormat_RGBA8Unorm;
    auto fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = shader; fragment.entryPoint = text("fs_main"); fragment.targetCount = 1; fragment.targets = &color;
    auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    descriptor.layout = depth_readback_layout; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main");
    descriptor.fragment = &fragment; descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    depth_readback_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuShaderModuleRelease(shader);
  }
  auto texture_descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
  texture_descriptor.size = {target.width, target.height, 1}; texture_descriptor.format = WGPUTextureFormat_RGBA8Unorm;
  texture_descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  auto packed = wgpuDeviceCreateTexture(device, &texture_descriptor);
  auto view_descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT; view_descriptor.aspect = WGPUTextureAspect_DepthOnly;
  auto input = wgpuTextureCreateView(target.handle, &view_descriptor);
  auto entry = WGPU_BIND_GROUP_ENTRY_INIT; entry.binding = 0; entry.textureView = input;
  auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  group_descriptor.layout = depth_readback_bindings; group_descriptor.entryCount = 1; group_descriptor.entries = &entry;
  auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
  auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  attachment.view = wgpuTextureCreateView(packed, nullptr); attachment.loadOp = WGPULoadOp_Clear; attachment.storeOp = WGPUStoreOp_Store;
  auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &attachment;
  auto pass = begin_pass(encoder, &descriptor);
  wgpuRenderPassEncoderSetPipeline(pass, depth_readback_pipeline); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
  wgpuTextureViewRelease(attachment.view); wgpuBindGroupRelease(group); wgpuTextureViewRelease(input);
  return packed;
}

void prepare_depth_upload() {
  if (depth_upload_pipeline) { return; }
  std::array<WGPUBindGroupLayoutEntry, 2> entries{};
  for (uint32_t n = 0; n < entries.size(); ++n) {
    entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[n].binding = n;
    entries[n].visibility = WGPUShaderStage_Fragment;
    entries[n].buffer.type = n ? WGPUBufferBindingType_Uniform : WGPUBufferBindingType_ReadOnlyStorage;
  }
  auto group = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  group.entryCount = entries.size(); group.entries = entries.data();
  depth_upload_bindings = wgpuDeviceCreateBindGroupLayout(device, &group);
  auto layout_descriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  layout_descriptor.bindGroupLayoutCount = 1; layout_descriptor.bindGroupLayouts = &depth_upload_bindings;
  depth_upload_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
  auto source = WGPU_SHADER_SOURCE_WGSL_INIT;
  source.code = text(R"(
struct Parameters { width: u32, maximum: u32, scale: vec2<f32> }
struct Vertex { @builtin(position) position: vec4<f32>, @location(0) @interpolate(flat) stencil: u32 }
@group(0) @binding(0) var<storage, read> pixels: array<u32>;
@group(0) @binding(1) var<uniform> parameters: Parameters;
@vertex fn vs_main(@builtin(vertex_index) index: u32, @builtin(instance_index) stencil: u32) -> Vertex {
  let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  var result: Vertex;
  result.position = vec4<f32>(positions[index], 0.0, 1.0);
  result.stencil = stencil;
  return result;
}
@fragment fn fs_main(input: Vertex) -> @builtin(frag_depth) f32 {
  let at = vec2<u32>(floor(input.position.xy * parameters.scale));
  let value = pixels[at.y * parameters.width + at.x];
  if ((value & 255u) != input.stencil) { discard; }
  let depth = value >> 8u;
  if (parameters.maximum == 65535u) { return unpack2x16unorm(depth).x; }
  let scaled = f32(depth) * 0.000000059604644775390625;
  return bitcast<f32>(bitcast<u32>(scaled) + select(0u, 1u, depth != 0u));
}
)");
  auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
  auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
  auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text("fs_main");
  auto depth = WGPU_DEPTH_STENCIL_STATE_INIT;
  depth.format = WGPUTextureFormat_Depth24PlusStencil8;
  depth.depthWriteEnabled = WGPUOptionalBool_True; depth.depthCompare = WGPUCompareFunction_Always;
  depth.stencilFront.compare = depth.stencilBack.compare = WGPUCompareFunction_Always;
  depth.stencilFront.passOp = depth.stencilBack.passOp = WGPUStencilOperation_Replace;
  depth.stencilReadMask = depth.stencilWriteMask = 255;
  auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  descriptor.layout = depth_upload_layout;
  descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main");
  descriptor.fragment = &fragment; descriptor.depthStencil = &depth;
  descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  depth_upload_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
  wgpuShaderModuleRelease(shader);
}
void upload_gamma() {
  if (!gamma_texture) {
    auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT; descriptor.size = {256, 1, 1}; descriptor.format = WGPUTextureFormat_RGBA8Unorm; descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    gamma_texture = wgpuDeviceCreateTexture(device, &descriptor);
  }
  std::array<uint8_t, 1024> bytes{};
  for (uint32_t n = 0; n < 256; ++n) { bytes[n * 4] = gamma_ramp[n]; bytes[n * 4 + 1] = gamma_ramp[n + 256]; bytes[n * 4 + 2] = gamma_ramp[n + 512]; bytes[n * 4 + 3] = 255; }
  auto destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; destination.texture = gamma_texture;
  auto layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT; layout.bytesPerRow = 1024; layout.rowsPerImage = 1;
  WGPUExtent3D extent{256, 1, 1}; wgpuQueueWriteTexture(queue, &destination, bytes.data(), bytes.size(), &layout, &extent);
}

// SMAA over the presented frame (src/gpu/smaa_wgsl.h): edges, blending weights, neighbourhood blending, each a
// fullscreen pass at the frame's size, before the gamma ramp as native's PrepareShownFrame
struct SmaaState {
  WGPUBindGroupLayout bindings = nullptr;
  WGPUPipelineLayout layout = nullptr;
  WGPURenderPipeline edges = nullptr, weights = nullptr, blend = nullptr;
  WGPUSampler linear = nullptr, point = nullptr;
  WGPUTexture area = nullptr, search = nullptr;
  WGPUTexture targets[3]{};  // edges, weights, output
  uint32_t width = 0, height = 0;
} smaa_state;
WGPUTexture smaa_texture(uint32_t width, uint32_t height, WGPUTextureFormat format, WGPUTextureUsage usage) {
  auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT; descriptor.size = {width, height, 1}; descriptor.format = format; descriptor.usage = usage;
  return wgpuDeviceCreateTexture(device, &descriptor);
}
void release_smaa_targets() {
  for (auto& target : smaa_state.targets) {
    if (target) { wgpuTextureDestroy(target); wgpuTextureRelease(target); target = nullptr; }
  }
  if (smaa_state.width) { texture_bytes -= uint64_t(smaa_state.width) * smaa_state.height * 10; }
  smaa_state.width = smaa_state.height = 0;
}
void release_smaa() {
  release_smaa_targets();
  for (auto* pipeline : {&smaa_state.edges, &smaa_state.weights, &smaa_state.blend}) { if (*pipeline) { wgpuRenderPipelineRelease(*pipeline); *pipeline = nullptr; } }
  for (auto* texture : {&smaa_state.area, &smaa_state.search}) { if (*texture) { wgpuTextureDestroy(*texture); wgpuTextureRelease(*texture); *texture = nullptr; } }
  for (auto* sampler : {&smaa_state.linear, &smaa_state.point}) { if (*sampler) { wgpuSamplerRelease(*sampler); *sampler = nullptr; } }
  if (smaa_state.layout) { wgpuPipelineLayoutRelease(smaa_state.layout); smaa_state.layout = nullptr; }
  if (smaa_state.bindings) { wgpuBindGroupLayoutRelease(smaa_state.bindings); smaa_state.bindings = nullptr; }
}
void prepare_smaa(uint32_t width, uint32_t height) {
  auto& state = smaa_state;
  if (!state.bindings) {
    std::array<WGPUBindGroupLayoutEntry, 6> entries{};
    for (uint32_t n = 0; n < entries.size(); ++n) {
      entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT; entries[n].binding = n; entries[n].visibility = WGPUShaderStage_Fragment;
    }
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment; entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].sampler.type = entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
    for (uint32_t n = 3; n < 6; ++n) { entries[n].texture.sampleType = WGPUTextureSampleType_Float; entries[n].texture.viewDimension = WGPUTextureViewDimension_2D; }
    auto group = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; group.entryCount = entries.size(); group.entries = entries.data();
    state.bindings = wgpuDeviceCreateBindGroupLayout(device, &group);
    auto layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; layout.bindGroupLayoutCount = 1; layout.bindGroupLayouts = &state.bindings;
    state.layout = wgpuDeviceCreatePipelineLayout(device, &layout);
    const auto pipeline = [&](const char* code, WGPUTextureFormat format) {
      auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = text(code);
      auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
      auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
      auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = format;
      auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text("fs_main"); fragment.targetCount = 1; fragment.targets = &target;
      auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT; descriptor.layout = state.layout; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main");
      descriptor.fragment = &fragment; descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      auto result = wgpuDeviceCreateRenderPipeline(device, &descriptor);
      wgpuShaderModuleRelease(shader);
      return result;
    };
    state.edges = pipeline(gpu::smaa::kEdgesWgsl, WGPUTextureFormat_RG8Unorm);
    state.weights = pipeline(gpu::smaa::kWeightsWgsl, WGPUTextureFormat_RGBA8Unorm);
    state.blend = pipeline(gpu::smaa::kBlendWgsl, WGPUTextureFormat_RGBA8Unorm);
    auto sampler = WGPU_SAMPLER_DESCRIPTOR_INIT; sampler.magFilter = sampler.minFilter = WGPUFilterMode_Linear;
    state.linear = wgpuDeviceCreateSampler(device, &sampler);
    sampler.magFilter = sampler.minFilter = WGPUFilterMode_Nearest;
    state.point = wgpuDeviceCreateSampler(device, &sampler);
    const auto upload = [](WGPUTexture texture, const uint8_t* bytes, uint32_t width, uint32_t height, uint32_t texel) {
      auto destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; destination.texture = texture;
      auto layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT; layout.bytesPerRow = width * texel; layout.rowsPerImage = height;
      WGPUExtent3D extent{width, height, 1}; wgpuQueueWriteTexture(queue, &destination, bytes, size_t(width) * height * texel, &layout, &extent);
    };
    state.area = smaa_texture(160, 560, WGPUTextureFormat_RG8Unorm, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    state.search = smaa_texture(64, 16, WGPUTextureFormat_R8Unorm, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    upload(state.area, gpu::kSmaaArea, 160, 560, 2);
    upload(state.search, gpu::kSmaaSearch, 64, 16, 1);
  }
  if (state.width == width && state.height == height) { return; }
  release_smaa_targets();
  const uint64_t bytes = uint64_t(width) * height * 10;
  if (texture_bytes + bytes > TextureBudget) { throw budget_exhausted("SMAA", bytes); }
  const auto usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
  state.targets[0] = smaa_texture(width, height, WGPUTextureFormat_RG8Unorm, usage);
  state.targets[1] = smaa_texture(width, height, WGPUTextureFormat_RGBA8Unorm, usage);
  state.targets[2] = smaa_texture(width, height, WGPUTextureFormat_RGBA8Unorm, usage);
  state.width = width; state.height = height; texture_bytes += bytes;
}
// the three passes over source; the blended frame is smaa_state.targets[2]
void run_smaa(WGPUCommandEncoder encoder, Texture& source) {
  prepare_smaa(source.width, source.height);
  auto& state = smaa_state;
  const std::array<float, 4> metrics{1.0f / float(source.width), 1.0f / float(source.height), float(source.width), float(source.height)};
  auto uniforms = buffer({reinterpret_cast<const uint8_t*>(metrics.data()), sizeof(metrics)}, WGPUBufferUsage_Uniform);
  auto color = wgpuTextureCreateView(source.handle, nullptr);
  std::array<WGPUTextureView, 3> views{};
  for (uint32_t n = 0; n < 3; ++n) { views[n] = wgpuTextureCreateView(state.targets[n], nullptr); }
  auto area = wgpuTextureCreateView(state.area, nullptr), search = wgpuTextureCreateView(state.search, nullptr);
  const auto pass = [&](WGPURenderPipeline pipeline, WGPUTextureView output, WGPUTextureView main, WGPUTextureView second) {
    std::array<WGPUBindGroupEntry, 6> entries{};
    for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_ENTRY_INIT; entries[n].binding = n; }
    entries[0].buffer = uniforms; entries[0].size = sizeof(metrics);
    entries[1].sampler = state.linear; entries[2].sampler = state.point;
    entries[3].textureView = main; entries[4].textureView = second; entries[5].textureView = search;
    auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = state.bindings; group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
    auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
    auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; attachment.view = output; attachment.loadOp = WGPULoadOp_Clear; attachment.storeOp = WGPUStoreOp_Store; attachment.clearValue = {0, 0, 0, 0};
    auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &attachment;
    auto render = begin_pass(encoder, &descriptor);
    wgpuRenderPassEncoderSetPipeline(render, pipeline); wgpuRenderPassEncoderSetBindGroup(render, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(render, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(render); wgpuRenderPassEncoderRelease(render);
    wgpuBindGroupRelease(group);
  };
  pass(state.edges, views[0], color, area);
  pass(state.weights, views[1], views[0], area);
  pass(state.blend, views[2], color, views[1]);
  for (auto view : views) { wgpuTextureViewRelease(view); }
  wgpuTextureViewRelease(color); wgpuTextureViewRelease(area); wgpuTextureViewRelease(search); wgpuBufferRelease(uniforms);
}
// Colour blind modes (native's ColourBlindMatrix): Machado 2009 simulation at full severity, then Fidaner's daltonize,
// which moves the difference the eye loses into channels it still sees. Rows of protanopia, deuteranopia, tritanopia.
constexpr float kColourBlindSimulate[3][9] = {
  {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
  {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
  {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f},
};
constexpr float kColourBlindShift[3][9] = {
  {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
  {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
  {1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0},
};
// mode 1 to 3: identity + shift * (identity - simulate), three rows of four with the first's w 1; 0 is all zero
std::array<float, 12> colour_blind_rows(uint32_t mode) {
  std::array<float, 12> rows{};
  if (!mode) { return rows; }
  const float* simulate = kColourBlindSimulate[mode - 1];
  const float* shift = kColourBlindShift[mode - 1];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      float lost = 0;
      for (int k = 0; k < 3; ++k) { lost += shift[r * 3 + k] * (float(k == c) - simulate[k * 3 + c]); }
      rows[size_t(r * 4 + c)] = float(r == c) + lost;
    }
  }
  rows[3] = 1;
  return rows;
}
void release_presentation_target() {
  if (presentation_target.handle) { texture_bytes -= uint64_t(presentation_target.width) * presentation_target.height * 4; wgpuTextureDestroy(presentation_target.handle); wgpuTextureRelease(presentation_target.handle); presentation_target = {}; }
}
void prepare_presentation() {
  if (!canvas_width || !canvas_height) { throw std::runtime_error("canvas not configured"); }
  if (!gamma_texture) { upload_gamma(); }
  if (!presentation_pipeline) {
    constexpr const char* code = R"(
struct Output { @builtin(position) position: vec4<f32>, @location(0) uv: vec2<f32> };
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var ramp: texture_2d<f32>;
// a colour blind mode's matrix by rows, applied to linear colour; r0.w 0 leaves colours alone
struct Colour { r0: vec4<f32>, r1: vec4<f32>, r2: vec4<f32> };
@group(0) @binding(2) var<uniform> colour: Colour;
@vertex fn vs_main(@builtin(vertex_index) vertex: u32) -> Output {
  let positions = array<vec2<f32>, 3>(vec2<f32>(-1,-1), vec2<f32>(3,-1), vec2<f32>(-1,3));
  let position = positions[vertex];
  var output: Output; output.position = vec4<f32>(position,0,1); output.uv = vec2<f32>((position.x+1)*0.5,(1-position.y)*0.5); return output;
}
fn ramped(color: vec4<f32>) -> vec4<f32> {
  let r = textureLoad(ramp,vec2<i32>(i32(round(clamp(color.r,0,1)*255)),0),0).r;
  let g = textureLoad(ramp,vec2<i32>(i32(round(clamp(color.g,0,1)*255)),0),0).g;
  let b = textureLoad(ramp,vec2<i32>(i32(round(clamp(color.b,0,1)*255)),0),0).b;
  if (colour.r0.w == 0) { return vec4<f32>(r,g,b,color.a); }
  let linear = pow(saturate(vec3<f32>(r,g,b)), vec3<f32>(2.2));
  let shifted = vec3<f32>(dot(colour.r0.xyz, linear), dot(colour.r1.xyz, linear), dot(colour.r2.xyz, linear));
  return vec4<f32>(pow(saturate(shifted), vec3<f32>(1.0 / 2.2)), color.a);
}
@fragment fn fs_main(input: Output) -> @location(0) vec4<f32> {
  let dimensions = textureDimensions(source);
  let at = clamp(vec2<i32>(floor(input.uv * vec2<f32>(dimensions))),vec2<i32>(0),vec2<i32>(dimensions)-vec2<i32>(1));
  return ramped(textureLoad(source,at,0));
}
@fragment fn fs_linear(input: Output) -> @location(0) vec4<f32> {
  let last = vec2<i32>(textureDimensions(source)) - vec2<i32>(1);
  let position = input.uv * vec2<f32>(textureDimensions(source)) - vec2<f32>(0.5);
  let corner = floor(position);
  let weight = position - corner;
  let a = clamp(vec2<i32>(corner), vec2<i32>(0), last);
  let b = clamp(vec2<i32>(corner) + vec2<i32>(1), vec2<i32>(0), last);
  let top = mix(textureLoad(source,a,0), textureLoad(source,vec2<i32>(b.x,a.y),0), weight.x);
  let bottom = mix(textureLoad(source,vec2<i32>(a.x,b.y),0), textureLoad(source,b,0), weight.x);
  return ramped(mix(top, bottom, weight.y));
})";
    auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = text(code);
    auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
    auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT; entries[n].binding = n; entries[n].visibility = WGPUShaderStage_Fragment; }
    for (uint32_t n = 0; n < 2; ++n) { entries[n].texture.sampleType = WGPUTextureSampleType_Float; entries[n].texture.viewDimension = WGPUTextureViewDimension_2D; }
    entries[2].buffer.type = WGPUBufferBindingType_Uniform;
    auto bindings = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bindings.entryCount = entries.size(); bindings.entries = entries.data(); presentation_bindings = wgpuDeviceCreateBindGroupLayout(device, &bindings);
    auto layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; layout.bindGroupLayoutCount = 1; layout.bindGroupLayouts = &presentation_bindings; presentation_layout = wgpuDeviceCreatePipelineLayout(device, &layout);
    auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = WGPUTextureFormat_RGBA8Unorm;
    auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text("fs_main"); fragment.targetCount = 1; fragment.targets = &target;
    auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT; descriptor.layout = presentation_layout; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main"); descriptor.fragment = &fragment;
    presentation_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    fragment.entryPoint = text("fs_linear"); presentation_linear_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor); wgpuShaderModuleRelease(shader);
  }
  if (presentation_target.width != canvas_width || presentation_target.height != canvas_height) {
    const uint64_t old_size = uint64_t(presentation_target.width) * presentation_target.height * 4;
    if (texture_bytes - old_size + uint64_t(canvas_width) * canvas_height * 4 > TextureBudget) { throw budget_exhausted("presentation", uint64_t(canvas_width) * canvas_height * 4); }
    release_presentation_target();
    auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT; descriptor.size = {canvas_width, canvas_height, 1}; descriptor.format = WGPUTextureFormat_RGBA8Unorm; descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding;
    presentation_target = {wgpuDeviceCreateTexture(device, &descriptor), canvas_width, canvas_height, false}; texture_bytes += uint64_t(canvas_width) * canvas_height * 4;
  }
}
void draw(uint32_t resource, std::span<const uint8_t> bytes, const DrawUploads* uploads = nullptr, uint32_t upload = 0) {
      auto spec = packet<DrawPacket>(bytes);
      const bool resident = spec.index32 == 2;
      const auto residentDraw = resident ? packet<ResidentDraw>(bytes) : ResidentDraw{};
      exact(bytes, uint64_t(spec.vertex_bytes) + spec.index_bytes + spec.uniform_bytes);
      for (const float value : spec.blend_constant) {
        if (!std::isfinite(value) || value < 0 || value > 1) { throw std::runtime_error("invalid blend constant"); }
      }
      auto found = pipelines.find(spec.pipeline); if (found == pipelines.end()) { throw std::runtime_error("unknown pipeline resource"); }
      const uint32_t stride = found->second.vertexStride;
      auto* target = resource ? &texture(resource) : nullptr;
      if ((!target && !spec.depth_target) || (target && (target->depth || target->cube || target->volumeDepth > 1)) || (!resident && (!spec.vertex_bytes || spec.vertex_bytes % stride || spec.vertex_count > spec.vertex_bytes / stride || spec.index32 > 1 || spec.index_bytes != uint64_t(spec.index_count) * (spec.index32 ? 4 : 2))) || (resident && (spec.vertex_bytes || spec.index_bytes || !spec.index_count || residentDraw.index_bytes != uint64_t(spec.index_count) * 4)) || !spec.uniform_bytes || spec.uniform_bytes > 65536 || spec.uniform_bytes % 16) { throw std::runtime_error("invalid draw payload"); }
      for (uint32_t n = 0; !resident && n < spec.index_count; ++n) {
        uint32_t index = 0;
        std::memcpy(&index, bytes.data() + spec.vertex_bytes + n * (spec.index32 ? 4 : 2), spec.index32 ? 4 : 2);
        if (index >= spec.vertex_bytes / stride) { throw std::runtime_error("draw index exceeds owned vertices"); }
      }
      for (uint32_t n = 0; n < 4; ++n) {
        if (spec.texture[n] == spec.depth_target || spec.texture[n] == resource) { throw std::runtime_error("invalid sampled texture resource"); }
        if (spec.sampler[n][0] < 1 || spec.sampler[n][0] > 3 || spec.sampler[n][1] < 1 || spec.sampler[n][1] > 3 || spec.sampler[n][2] < 1 || spec.sampler[n][2] > 2 || spec.sampler[n][3] < 1 || spec.sampler[n][3] > 2 || spec.sampler[n][4] < 1 || spec.sampler[n][4] > 2 || spec.sampler[n][5] > spec.sampler[n][6] || spec.sampler[n][6] > 32 || spec.address_w[n] < 1 || spec.address_w[n] > 3 ||
            spec.anisotropy[n] < 1 || spec.anisotropy[n] > 16 || (spec.anisotropy[n] > 1 && (spec.sampler[n][2] != 2 || spec.sampler[n][3] != 2 || spec.sampler[n][4] != 2))) { throw std::runtime_error("unsupported sampler state"); }
      }
      if (spec.depth_target && !texture(spec.depth_target).depth) { throw std::runtime_error("invalid depth target resource"); }
      std::array<WGPUBuffer, 16> residentVertices{};
      WGPUBuffer residentIndices = nullptr;
      if (resident) {
        const auto index = resident_buffers.find(residentDraw.indices);
        if (index == resident_buffers.end() || uint64_t(residentDraw.index_offset) + residentDraw.index_bytes > index->second.bytes || residentDraw.index_offset % 4) { throw std::runtime_error("invalid resident index buffer"); }
        residentIndices = index->second.handle;
        for (uint32_t n = 0; n < 16; ++n) {
          if (!residentDraw.vertices[n]) { continue; }
          const auto vertex = resident_buffers.find(residentDraw.vertices[n]);
          if (n >= 8 || vertex == resident_buffers.end() || !residentDraw.sizes[n] || uint64_t(residentDraw.offsets[n]) + residentDraw.sizes[n] > vertex->second.bytes || residentDraw.offsets[n] % 4) { throw std::runtime_error("invalid resident vertex buffer"); }
          residentVertices[n] = vertex->second.handle;
        }
      }
      const std::array<uint64_t, 3> offsets = uploads ? uploads->offsets[upload] : std::array<uint64_t, 3>{};
      auto vertices = resident ? nullptr : uploads ? uploads->buffers[0] : buffer(bytes.first(spec.vertex_bytes), WGPUBufferUsage_Vertex);
      auto uniforms = uploads ? uploads->buffers[2] : buffer(bytes.subspan(spec.vertex_bytes + spec.index_bytes), WGPUBufferUsage_Uniform);
      WGPUBuffer indices = nullptr;
      if (resident) { indices = residentIndices; } else if (spec.index_count) { indices = uploads ? uploads->buffers[1] : buffer(bytes.subspan(spec.vertex_bytes, spec.index_bytes), WGPUBufferUsage_Index); }
      std::array<WGPUBindGroupEntry, 9> entries{}; std::array<WGPUTextureView, 4> views{}; std::array<WGPUSampler, 4> samplers{};
      entries[0] = WGPU_BIND_GROUP_ENTRY_INIT; entries[0].binding = 0; entries[0].buffer = uniforms; entries[0].offset = offsets[2]; entries[0].size = spec.uniform_bytes;
      for (uint32_t n = 0; n < 4; ++n) {
        auto& sampled = texture(spec.texture[n]);
        readable(sampled);
        samplers[n] = cached_sampler(spec.sampler[n], spec.address_w[n], spec.anisotropy[n]);
        views[n] = sampled_view(sampled);
        entries[1 + n * 2] = WGPU_BIND_GROUP_ENTRY_INIT; entries[1 + n * 2].binding = 1 + n * 2; entries[1 + n * 2].textureView = views[n];
        entries[2 + n * 2] = WGPU_BIND_GROUP_ENTRY_INIT; entries[2 + n * 2].binding = 2 + n * 2; entries[2 + n * 2].sampler = samplers[n];
      }
      auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = wgpuRenderPipelineGetBindGroupLayout(found->second.handle, 0); group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
      auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
      wgpuBindGroupLayoutRelease(group_descriptor.layout);
      auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
      auto pass_descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
      auto* depth_texture = spec.depth_target ? &texture(spec.depth_target) : nullptr;
      if (target) {
        // a multisampled target draws into its companion and resolves into the texture as the pass ends
        attachment.view = target->ms ? target->msView : attachment_view(*target);
        if (target->ms) { attachment.resolveTarget = attachment_view(*target); }
        attachment.loadOp = WGPULoadOp_Load; attachment.storeOp = WGPUStoreOp_Store;
        pass_descriptor.colorAttachmentCount = 1; pass_descriptor.colorAttachments = &attachment;
      }
      auto depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
      if (depth_texture) { depth.depthClearValue = 1; depth.view = depth_texture->ms ? depth_texture->msView : attachment_view(*depth_texture); depth.depthLoadOp = WGPULoadOp_Load; depth.depthStoreOp = WGPUStoreOp_Store; depth.stencilLoadOp = WGPULoadOp_Load; depth.stencilStoreOp = WGPUStoreOp_Store; pass_descriptor.depthStencilAttachment = &depth; }
      auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
      if (batch_encoder && batch_pass && (batch_color != resource || batch_depth != spec.depth_target)) { end_batch_pass(); }
      const bool reuse = batch_encoder && batch_pass;
      if (!reuse) {
        if (target) { seed(encoder, *target); }
        if (depth_texture) { seed(encoder, *depth_texture); }
      }
      if (depth_texture && depth_texture->ms) { depth_texture->stale = true; }
      auto pass = reuse ? batch_pass : begin_pass(encoder, &pass_descriptor);
      if (batch_encoder) { batch_pass = pass; batch_color = resource; batch_depth = spec.depth_target; }
      wgpuRenderPassEncoderSetPipeline(pass, found->second.handle); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr); if (resident) {
        for (uint32_t n = 0; n < 16; ++n) {
          if (!residentDraw.vertices[n]) { continue; }
          wgpuRenderPassEncoderSetVertexBuffer(pass, n, residentVertices[n], residentDraw.offsets[n], residentDraw.sizes[n]);
        }
      } else { wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertices, offsets[0], spec.vertex_bytes); }
      wgpuRenderPassEncoderSetStencilReference(pass, spec.stencil_reference);
      const WGPUColor blendConstant{spec.blend_constant[0], spec.blend_constant[1], spec.blend_constant[2], spec.blend_constant[3]};
      wgpuRenderPassEncoderSetBlendConstant(pass, &blendConstant);
      wgpuRenderPassEncoderSetViewport(pass, spec.viewport[0], spec.viewport[1], spec.viewport[2], spec.viewport[3], spec.viewport[4], spec.viewport[5]); wgpuRenderPassEncoderSetScissorRect(pass, spec.scissor[0], spec.scissor[1], spec.scissor[2], spec.scissor[3]);
      if (indices) { wgpuRenderPassEncoderSetIndexBuffer(pass, indices, spec.index32 ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16, resident ? residentDraw.index_offset : offsets[1], resident ? residentDraw.index_bytes : spec.index_bytes); wgpuRenderPassEncoderDrawIndexed(pass, spec.index_count, 1, 0, 0, 0); }
      else { wgpuRenderPassEncoderDraw(pass, spec.vertex_count, 1, 0, 0); }
      if (!batch_encoder) { wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass); submit(encoder); }
      for (auto item : samplers) { wgpuSamplerRelease(item); }
      wgpuBindGroupRelease(group);
      if (!uploads) { if (vertices) { wgpuBufferRelease(vertices); } wgpuBufferRelease(uniforms); if (indices && !resident) { wgpuBufferRelease(indices); } }
}

// 0 seeds a colour companion from its texture, 1 a depth companion from its texture, 2 resolves a depth companion's
// sample 0 into its texture, 3 seeds an HDR colour companion
WGPUBindGroupLayout msaa_bindings[4];
WGPUPipelineLayout msaa_layouts[4];
WGPURenderPipeline msaa_pipelines[4];
WGPURenderPipeline msaa_pipeline(uint32_t kind) {
  if (msaa_pipelines[kind]) { return msaa_pipelines[kind]; }
  auto entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT; entry.binding = 0; entry.visibility = WGPUShaderStage_Fragment;
  const bool colour = kind == 0 || kind == 3;
  entry.texture.sampleType = colour ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Depth;
  entry.texture.viewDimension = WGPUTextureViewDimension_2D; entry.texture.multisampled = kind == 2;
  auto group = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; group.entryCount = 1; group.entries = &entry;
  msaa_bindings[kind] = wgpuDeviceCreateBindGroupLayout(device, &group);
  auto layout_descriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; layout_descriptor.bindGroupLayoutCount = 1; layout_descriptor.bindGroupLayouts = &msaa_bindings[kind];
  msaa_layouts[kind] = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
  std::string code = R"(
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
  let positions = array<vec2<f32>, 3>(vec2<f32>(-1,-1), vec2<f32>(3,-1), vec2<f32>(-1,3));
  return vec4<f32>(positions[index], 0, 1);
}
)";
  if (colour) { code += "@group(0) @binding(0) var image: texture_2d<f32>;\n@fragment fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> { return textureLoad(image, vec2<i32>(position.xy), 0); }\n"; }
  if (kind == 1) { code += "@group(0) @binding(0) var image: texture_depth_2d;\n@fragment fn fs_main(@builtin(position) position: vec4<f32>) -> @builtin(frag_depth) f32 { return textureLoad(image, vec2<i32>(position.xy), 0); }\n"; }
  if (kind == 2) { code += "@group(0) @binding(0) var image: texture_depth_multisampled_2d;\n@fragment fn fs_main(@builtin(position) position: vec4<f32>) -> @builtin(frag_depth) f32 { return textureLoad(image, vec2<i32>(position.xy), 0); }\n"; }
  auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = {code.data(), code.size()};
  auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
  auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
  auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  descriptor.layout = msaa_layouts[kind]; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main");
  descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  descriptor.multisample.count = kind == 2 ? 1 : 4;
  auto color = WGPU_COLOR_TARGET_STATE_INIT; color.format = kind == 3 ? hdr_format : WGPUTextureFormat_RGBA8Unorm;
  auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text("fs_main");
  if (colour) { fragment.targetCount = 1; fragment.targets = &color; }
  descriptor.fragment = &fragment;
  auto depth = WGPU_DEPTH_STENCIL_STATE_INIT;
  if (!colour) {
    depth.format = WGPUTextureFormat_Depth24PlusStencil8; depth.depthWriteEnabled = WGPUOptionalBool_True; depth.depthCompare = WGPUCompareFunction_Always;
    depth.stencilReadMask = depth.stencilWriteMask = 0;
    descriptor.depthStencil = &depth;
  }
  msaa_pipelines[kind] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
  wgpuShaderModuleRelease(shader);
  return msaa_pipelines[kind];
}
void release_msaa() {
  for (auto& pipeline : msaa_pipelines) { if (pipeline) { wgpuRenderPipelineRelease(pipeline); pipeline = nullptr; } }
  for (auto& layout : msaa_layouts) { if (layout) { wgpuPipelineLayoutRelease(layout); layout = nullptr; } }
  for (auto& group : msaa_bindings) { if (group) { wgpuBindGroupLayoutRelease(group); group = nullptr; } }
}
// one fullscreen draw from source's whole texture (or companion) into target's
void msaa_pass(WGPUCommandEncoder encoder, uint32_t kind, WGPUTextureView input, WGPUTextureView output, bool depth) {
  auto entry = WGPU_BIND_GROUP_ENTRY_INIT; entry.binding = 0; entry.textureView = input;
  auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = msaa_bindings[kind]; group_descriptor.entryCount = 1; group_descriptor.entries = &entry;
  const auto pipeline = msaa_pipeline(kind);
  group_descriptor.layout = msaa_bindings[kind];
  auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
  auto color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; auto attachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
  auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  if (depth) {
    attachment.view = output; attachment.depthLoadOp = attachment.stencilLoadOp = WGPULoadOp_Load; attachment.depthStoreOp = attachment.stencilStoreOp = WGPUStoreOp_Store;
    attachment.depthClearValue = 0; attachment.stencilClearValue = 0;
    descriptor.depthStencilAttachment = &attachment;
  } else {
    color.view = output; color.loadOp = WGPULoadOp_Load; color.storeOp = WGPUStoreOp_Store;
    descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &color;
  }
  auto pass = begin_pass(encoder, &descriptor);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
  wgpuBindGroupRelease(group);
}
WGPUTextureView depth_only(WGPUTexture handle) {
  auto descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT; descriptor.aspect = WGPUTextureAspect_DepthOnly;
  return wgpuTextureCreateView(handle, &descriptor);
}
// the companion takes the texture's texels (every sample the texel) before a pass draws into it
void seed(WGPUCommandEncoder encoder, Texture& texture) {
  if (!texture.ms || !texture.seed) { return; }
  texture.seed = false;
  ++frame_stats[StatSeeds];
  auto input = texture.depth ? depth_only(texture.handle) : wgpuTextureCreateView(texture.handle, nullptr);
  msaa_pass(encoder, texture.depth ? 1 : texture.hdr ? 3 : 0, input, texture.msView, texture.depth);
  wgpuTextureViewRelease(input);
}
// a stale depth texture takes its companion's sample 0 before anything reads it
void readable(Texture& texture) {
  if (!texture.ms || !texture.stale) { return; }
  texture.stale = false;
  ++frame_stats[StatDepthResolves];
  end_batch_pass();
  auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
  auto input = depth_only(texture.ms);
  msaa_pass(encoder, 2, input, attachment_view(texture), true);
  wgpuTextureViewRelease(input);
  if (!batch_encoder) { submit(encoder); }
}
void multisample(uint32_t resource, std::span<const uint8_t> bytes) {
  const uint32_t samples = packet<uint32_t>(bytes); exact(bytes, 0);
  auto& target = texture(resource);
  if ((samples != 1 && samples != 4) || target.cube || target.volumeDepth > 1 || target.levels != 1) { throw std::runtime_error("invalid multisample request"); }
  if ((samples == 4) == bool(target.ms)) { return; }
  end_batch_pass();
  if (samples == 1) {
    readable(target);
    wgpuTextureViewRelease(target.msView); target.msView = nullptr;
    wgpuTextureDestroy(target.ms); wgpuTextureRelease(target.ms); target.ms = nullptr;
    texture_bytes -= multisample_bytes(target);
    return;
  }
  const uint64_t bytesNeeded = multisample_bytes(target);
  if (texture_bytes + bytesNeeded > TextureBudget) { throw budget_exhausted("multisample", bytesNeeded); }
  auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
  descriptor.size = {target.width, target.height, 1}; descriptor.sampleCount = 4;
  descriptor.format = target.depth ? WGPUTextureFormat_Depth24PlusStencil8 : color_format(target);
  descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
  target.ms = wgpuDeviceCreateTexture(device, &descriptor);
  target.msView = wgpuTextureCreateView(target.ms, nullptr);
  texture_bytes += bytesNeeded;
  target.seed = true; target.stale = false;
}

// Chaos Theory's shader model 3 soft shadows (src/gpu/d3d12.cpp's kSoftShadowHlsl in WGSL): a 256x256 copy of view
// depth and shadow, a 128x128 quarter and penumbra mask, the masked pixels filtered by a depth aware disk of 8 taps,
// both grown with the integer render scale; then ApplyShadows writes the result over the scene's alpha
constexpr const char* kSoftShadowWgsl = R"(
struct Frame { zp: vec4<f32>, vp: vec4<f32>, sz: vec4<f32> }
@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var depth: DEPTH_TYPE;
@group(0) @binding(2) var scene: texture_2d<f32>;
@group(0) @binding(3) var small: texture_2d<f32>;
@group(0) @binding(4) var quarter: texture_2d<f32>;
@group(0) @binding(5) var bilinear: sampler;
const kDepthRadius = 0.02;
const kDiskRadius = 3.2;
const kDiskDepthScale = 0.6;
const kDiskDepthBias = -0.4;
const kBlendDepthScale = 1.2;
const kBlendDepthBias = -0.3;
const kNearOffset = 0.1;
const kCentreWeight = 0.001;
const kPenumbraLow = 0.000784;
const kPenumbraHigh = 0.99922;
const kDisk = array<vec2<f32>, 8>(vec2<f32>(0, 0), vec2<f32>(0.527837, -0.085868), vec2<f32>(-0.040088, 0.536087),
  vec2<f32>(-0.670445, -0.179949), vec2<f32>(-0.419418, -0.616039), vec2<f32>(0.440453, -0.639399),
  vec2<f32>(-0.757088, 0.349334), vec2<f32>(0.574619, 0.685879));
const kCorners = array<vec2<f32>, 4>(vec2<f32>(-1, -1), vec2<f32>(1, -1), vec2<f32>(-1, 1), vec2<f32>(1, 1));
const kDiskWeight = array<f32, 8>(1, 0.7140148, 0.7110037, 0.5181218, 0.4445845, 0.3971701, 0.3047835, 0.1993830);
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
  let positions = array<vec2<f32>, 3>(vec2<f32>(-1,-1), vec2<f32>(3,-1), vec2<f32>(-1,3));
  return vec4<f32>(positions[index], 0, 1);
}
fn inside(p: vec2<i32>) -> vec2<i32> { return clamp(p, vec2<i32>(frame.vp.xy), vec2<i32>(frame.vp.xy + frame.vp.zw) - vec2<i32>(1)); }
fn view_depth(p: vec2<i32>) -> f32 {
  let z = frame.zp.x / (textureLoad(depth, inside(p), 0) - frame.zp.y);
  return saturate(z * 0.0010001 - 0.00010001);
}
fn depth_shadow(p: vec2<i32>) -> vec2<f32> { return vec2<f32>(view_depth(p), textureLoad(scene, inside(p), 0).a); }
fn bilinear_at(at: vec2<f32>) -> vec2<f32> {
  let f = at - vec2<f32>(0.5);
  let i = vec2<i32>(floor(f));
  let t = f - floor(f);
  let top = mix(depth_shadow(i), depth_shadow(i + vec2<i32>(1, 0)), t.x);
  let bottom = mix(depth_shadow(i + vec2<i32>(0, 1)), depth_shadow(i + vec2<i32>(1, 1)), t.x);
  return mix(top, bottom, t.y);
}
@fragment fn shadow_small(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let at = frame.vp.xy + pos.xy / frame.sz.x * frame.vp.zw;
  let tap = 0.4 / 256.0 * frame.vp.zw;
  var sum = vec2<f32>(0);
  for (var i = 0; i < 4; i++) { sum += bilinear_at(at + kCorners[i] * tap); }
  let mean = sum * 0.25;
  return vec4<f32>(mean.x / kDepthRadius, mean.y, 0, 0);
}
@fragment fn shadow_quarter(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  var s = 0.0;
  for (var i = 0; i < 4; i++) { s += textureSampleLevel(small, bilinear, pos.xy / frame.sz.y + kCorners[i] * 0.5 / 256.0, 0.0).y; }
  return vec4<f32>(s * 0.25);
}
@fragment fn shadow_penumbra(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  var s = 0.0;
  for (var i = 0; i < 4; i++) { s += textureSampleLevel(quarter, bilinear, pos.xy / frame.sz.y + kCorners[i] * 0.5 / 128.0, 0.0).x; }
  s *= 0.25;
  return vec4<f32>(select(0.0, 1.0, s >= kPenumbraLow && s < kPenumbraHigh));
}
@fragment fn shadow_filter(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let p = vec2<i32>(pos.xy);
  let uv = (pos.xy - frame.vp.xy) / frame.vp.zw;
  let lit = textureLoad(scene, p, 0).a;
  let cell = min(vec2<i32>(uv * frame.sz.y), vec2<i32>(i32(frame.sz.y) - 1));
  let plain = lit * textureSampleLevel(small, bilinear, uv, 0.0).y;
  let d = view_depth(p);
  let nearness = 1.0 / (d + kNearOffset);
  let radius = saturate(nearness * kDiskDepthScale + kDiskDepthBias) * kDiskRadius / 256.0;
  let blend = saturate(nearness * kBlendDepthScale + kBlendDepthBias);
  let own = d / kDepthRadius;
  var sum = lit * kCentreWeight;
  var weights = kCentreWeight;
  for (var i = 0; i < 8; i++) {
    let tap = textureSampleLevel(small, bilinear, uv + kDisk[i] * radius, 0.0).xy;
    let apart = tap.x - own;
    let w = saturate(kDiskWeight[i] - apart * apart);
    sum += tap.y * w;
    weights += w;
  }
  return vec4<f32>(select(mix(lit, sum / weights, blend), plain, textureLoad(quarter, cell, 0).x <= 0.5));
}
@fragment fn shadow_apply(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  return vec4<f32>(0, 0, 0, textureLoad(scene, vec2<i32>(pos.xy), 0).x);
}
)";
struct SoftShadowState {
  WGPUBindGroupLayout bindings[2]{};
  WGPUPipelineLayout layouts[2]{};
  std::map<std::pair<uint32_t, uint32_t>, WGPURenderPipeline> pipelines;  // (pass, 1 multisampled depth or 4x target | 2 HDR scene)
  WGPUTexture targets[4]{};  // small, quarter, penumbra, filtered
  WGPUSampler sampler = nullptr;
  WGPUTexture dummy = nullptr, dummyDepth[2]{};
  uint32_t width = 0, height = 0, scale = 1;
  uint32_t scene = 0;  // the scene the filtered shadow is for, 0 when none waits
  float rect[4]{};
} soft_state;
constexpr WGPUTextureFormat kSoftFormats[4] = {WGPUTextureFormat_RG16Float, WGPUTextureFormat_R16Float, WGPUTextureFormat_R16Float, WGPUTextureFormat_R16Float};
constexpr const char* kSoftEntries[5] = {"shadow_small", "shadow_quarter", "shadow_penumbra", "shadow_filter", "shadow_apply"};
WGPURenderPipeline soft_pipeline(uint32_t pass, bool multisampled, bool hdr) {
  auto& state = soft_state;
  const auto key = std::pair{pass, uint32_t(multisampled) | (pass == 4 && hdr ? 2u : 0u)};
  if (const auto found = state.pipelines.find(key); found != state.pipelines.end()) { return found->second; }
  const bool depthMs = multisampled && pass != 4;
  if (!state.bindings[depthMs]) {
    std::array<WGPUBindGroupLayoutEntry, 6> entries{};
    for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT; entries[n].binding = n; entries[n].visibility = WGPUShaderStage_Fragment; }
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].texture.sampleType = WGPUTextureSampleType_Depth; entries[1].texture.viewDimension = WGPUTextureViewDimension_2D; entries[1].texture.multisampled = depthMs;
    for (uint32_t n = 2; n < 5; ++n) { entries[n].texture.sampleType = WGPUTextureSampleType_Float; entries[n].texture.viewDimension = WGPUTextureViewDimension_2D; }
    entries[5].sampler.type = WGPUSamplerBindingType_Filtering;
    auto group = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; group.entryCount = entries.size(); group.entries = entries.data();
    state.bindings[depthMs] = wgpuDeviceCreateBindGroupLayout(device, &group);
    auto layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; layout.bindGroupLayoutCount = 1; layout.bindGroupLayouts = &state.bindings[depthMs];
    state.layouts[depthMs] = wgpuDeviceCreatePipelineLayout(device, &layout);
  }
  std::string code = std::string(kSoftShadowWgsl);
  const std::string marker = "DEPTH_TYPE";
  code.replace(code.find(marker), marker.size(), depthMs ? "texture_depth_multisampled_2d" : "texture_depth_2d");
  auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = {code.data(), code.size()};
  auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
  auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
  auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = pass == 4 ? (hdr ? hdr_format : WGPUTextureFormat_RGBA8Unorm) : kSoftFormats[pass];
  if (pass == 4) { target.writeMask = WGPUColorWriteMask_Alpha; }
  auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text(kSoftEntries[pass]); fragment.targetCount = 1; fragment.targets = &target;
  auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT; descriptor.layout = state.layouts[depthMs]; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main");
  descriptor.fragment = &fragment; descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  if (pass == 4 && multisampled) { descriptor.multisample.count = 4; }
  auto pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
  wgpuShaderModuleRelease(shader);
  state.pipelines.emplace(key, pipeline);
  return pipeline;
}
void release_soft_targets() {
  for (auto& target : soft_state.targets) { if (target) { wgpuTextureDestroy(target); wgpuTextureRelease(target); target = nullptr; } }
  soft_state.width = soft_state.height = 0;
}
void release_soft() {
  release_soft_targets();
  for (auto& [key, pipeline] : soft_state.pipelines) { wgpuRenderPipelineRelease(pipeline); }
  soft_state.pipelines.clear();
  for (auto& layout : soft_state.layouts) { if (layout) { wgpuPipelineLayoutRelease(layout); layout = nullptr; } }
  for (auto& group : soft_state.bindings) { if (group) { wgpuBindGroupLayoutRelease(group); group = nullptr; } }
  for (auto* texture : {&soft_state.dummy, &soft_state.dummyDepth[0], &soft_state.dummyDepth[1]}) { if (*texture) { wgpuTextureDestroy(*texture); wgpuTextureRelease(*texture); *texture = nullptr; } }
  if (soft_state.sampler) { wgpuSamplerRelease(soft_state.sampler); soft_state.sampler = nullptr; }
  soft_state.scene = 0;
}
WGPUTexture post_texture(uint32_t width, uint32_t height, WGPUTextureFormat format, uint32_t samples = 1) {
  auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT; descriptor.size = {width, height, 1}; descriptor.format = format; descriptor.sampleCount = samples;
  descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
  return wgpuDeviceCreateTexture(device, &descriptor);
}
// one fullscreen pass of the soft shadow module into output (scissored to rect when given)
void soft_pass(WGPUCommandEncoder encoder, uint32_t pass, bool multisampled, WGPUBuffer uniforms, WGPUTextureView depth, WGPUTextureView scene,
               WGPUTextureView small, WGPUTextureView quarter, WGPUTextureView output, WGPUTextureView resolve, const float* rect, bool load, bool hdr = false) {
  auto& state = soft_state;
  const auto pipeline = soft_pipeline(pass, multisampled, hdr);
  std::array<WGPUBindGroupEntry, 6> entries{};
  for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_ENTRY_INIT; entries[n].binding = n; }
  entries[0].buffer = uniforms; entries[0].size = 48;
  entries[1].textureView = depth; entries[2].textureView = scene; entries[3].textureView = small; entries[4].textureView = quarter;
  entries[5].sampler = state.sampler;
  auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = state.bindings[multisampled && pass != 4]; group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
  auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
  auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; attachment.view = output; attachment.resolveTarget = resolve;
  attachment.loadOp = load ? WGPULoadOp_Load : WGPULoadOp_Clear; attachment.storeOp = WGPUStoreOp_Store; attachment.clearValue = {0, 0, 0, 0};
  auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &attachment;
  auto render = begin_pass(encoder, &descriptor);
  wgpuRenderPassEncoderSetPipeline(render, pipeline); wgpuRenderPassEncoderSetBindGroup(render, 0, group, 0, nullptr);
  if (rect) { wgpuRenderPassEncoderSetScissorRect(render, uint32_t(rect[0]), uint32_t(rect[1]), uint32_t(rect[2]), uint32_t(rect[3])); }
  wgpuRenderPassEncoderDraw(render, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(render); wgpuRenderPassEncoderRelease(render);
  wgpuBindGroupRelease(group);
}
WGPUTextureView dummy_view(bool depth, bool multisampled) {
  auto& state = soft_state;
  if (!state.dummy) {
    state.dummy = post_texture(1, 1, WGPUTextureFormat_RGBA8Unorm);
    state.dummyDepth[0] = post_texture(1, 1, WGPUTextureFormat_Depth24PlusStencil8);
    state.dummyDepth[1] = post_texture(1, 1, WGPUTextureFormat_Depth24PlusStencil8, 4);
  }
  return depth ? depth_only(state.dummyDepth[multisampled]) : wgpuTextureCreateView(state.dummy, nullptr);
}
void soft_shadows(uint32_t resource, std::span<const uint8_t> bytes) {
  const auto spec = packet<SoftShadowPacket>(bytes); exact(bytes, 0);
  auto& scene = texture(resource); auto& depth = texture(spec.depth);
  if (scene.depth || !depth.depth || scene.width != depth.width || scene.height != depth.height || bool(scene.ms) != bool(depth.ms) ||
      !(spec.vp[2] > 0) || !(spec.vp[3] > 0) || spec.sz[0] < 1 || spec.sz[1] < 1 || spec.sz[0] > 4096 || spec.sz[1] > 4096) {
    throw std::runtime_error("invalid soft shadow request");
  }
  auto& state = soft_state;
  end_batch_pass();
  const uint32_t copy = uint32_t(spec.sz[0]), mask = uint32_t(spec.sz[1]);
  if (state.width != scene.width || state.height != scene.height || state.scale != copy) {
    release_soft_targets();
    state.targets[0] = post_texture(copy, copy, kSoftFormats[0]);
    state.targets[1] = post_texture(mask, mask, kSoftFormats[1]);
    state.targets[2] = post_texture(mask, mask, kSoftFormats[2]);
    state.targets[3] = post_texture(scene.width, scene.height, kSoftFormats[3]);
    state.width = scene.width; state.height = scene.height; state.scale = copy;
  }
  if (!state.sampler) {
    auto descriptor = WGPU_SAMPLER_DESCRIPTOR_INIT; descriptor.magFilter = descriptor.minFilter = WGPUFilterMode_Linear;
    state.sampler = wgpuDeviceCreateSampler(device, &descriptor);
  }
  const bool multisampled = bool(depth.ms);
  std::array<float, 12> frame{};
  std::copy(std::begin(spec.zp), std::end(spec.zp), frame.begin());
  std::copy(std::begin(spec.vp), std::end(spec.vp), frame.begin() + 4);
  std::copy(std::begin(spec.sz), std::end(spec.sz), frame.begin() + 8);
  auto uniforms = buffer({reinterpret_cast<const uint8_t*>(frame.data()), sizeof(frame)}, WGPUBufferUsage_Uniform);
  auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
  auto depthView = depth_only(multisampled ? depth.ms : depth.handle);
  auto sceneView = wgpuTextureCreateView(scene.handle, nullptr);
  std::array<WGPUTextureView, 4> views{};
  for (uint32_t n = 0; n < 4; ++n) { views[n] = wgpuTextureCreateView(state.targets[n], nullptr); }
  auto blank = dummy_view(false, false);
  soft_pass(encoder, 0, multisampled, uniforms, depthView, sceneView, blank, blank, views[0], nullptr, nullptr, false);
  auto blankDepth = dummy_view(true, multisampled);
  soft_pass(encoder, 1, multisampled, uniforms, blankDepth, blank, views[0], blank, views[1], nullptr, nullptr, false);
  soft_pass(encoder, 2, multisampled, uniforms, blankDepth, blank, blank, views[1], views[2], nullptr, nullptr, false);
  soft_pass(encoder, 3, multisampled, uniforms, depthView, sceneView, views[0], views[2], views[3], nullptr, spec.vp, false);
  if (!batch_encoder) { submit(encoder); }
  for (auto view : views) { wgpuTextureViewRelease(view); }
  wgpuTextureViewRelease(depthView); wgpuTextureViewRelease(sceneView); wgpuTextureViewRelease(blank); wgpuTextureViewRelease(blankDepth);
  wgpuBufferRelease(uniforms);
  state.scene = resource;
  std::copy(std::begin(spec.vp), std::end(spec.vp), state.rect);
}
void apply_shadows(uint32_t resource, std::span<const uint8_t> bytes) {
  exact(bytes, 0);
  auto& state = soft_state;
  auto& scene = texture(resource);
  const bool pending = state.scene == resource && scene.width == state.width && scene.height == state.height;
  state.scene = 0;
  if (!pending) { return; }
  end_batch_pass();
  const bool multisampled = bool(scene.ms);
  std::array<float, 12> frame{};
  auto uniforms = buffer({reinterpret_cast<const uint8_t*>(frame.data()), sizeof(frame)}, WGPUBufferUsage_Uniform);
  auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
  if (multisampled) { seed(encoder, scene); }
  auto filtered = wgpuTextureCreateView(state.targets[3], nullptr);
  auto blank = dummy_view(false, false), blankDepth = dummy_view(true, false);
  // a multisampled scene takes the alpha in every sample and resolves, as its draws do
  soft_pass(encoder, 4, multisampled, uniforms, blankDepth, filtered, blank, blank, multisampled ? scene.msView : attachment_view(scene),
    multisampled ? attachment_view(scene) : nullptr, state.rect, true, scene.hdr);
  if (!batch_encoder) { submit(encoder); }
  wgpuTextureViewRelease(filtered); wgpuTextureViewRelease(blank); wgpuTextureViewRelease(blankDepth); wgpuBufferRelease(uniforms);
}
// native's HDR resolve (src/gpu/d3d12.cpp's kPostHlsl in WGSL): Chaos Theory's eye adaptation over a grid, a quarter
// size bloom of what passes white blurred across then down, then scene and glow times the exposure scale
constexpr const char* kResolveWgsl = R"(
struct P { p0: vec4<f32>, p1: vec4<f32>, p2: vec4<f32> }
@group(0) @binding(0) var<uniform> p: P;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var bloom: texture_2d<f32>;
@group(0) @binding(3) var expo: texture_2d<f32>;
@group(0) @binding(4) var ls: sampler;
struct O { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> }
@vertex fn vs_main(@builtin(vertex_index) id: u32) -> O {
  let t = vec2<f32>(f32((id << 1u) & 2u), f32(id & 2u));
  var o: O;
  o.pos = vec4<f32>(t * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
  o.uv = t;
  return o;
}
const kLum = vec3<f32>(0.3, 0.59, 0.11);
fn exposure_now() -> f32 { return textureLoad(expo, vec2<i32>(0), 0).r; }
// Chaos Theory's eye adaptation, the scale the scene goes to. p0: headroom, glow strength, glow start, brightness
// max; p1: brightness min, dark end, normal start, normal end; p2: bright start, dark, normal and bright scale
@fragment fn exposure(i: O) -> @location(0) vec4<f32> {
  var s = 0.0;
  for (var y = 0; y < 18; y++) {
    for (var x = 0; x < 32; x++) {
      let at = (vec2<f32>(f32(x), f32(y)) + vec2<f32>(0.5)) / vec2<f32>(32.0, 18.0);
      let c = textureSampleLevel(src, ls, at, 0.0).rgb * p.p0.x;
      let lit = dot(min(c, vec3<f32>(1.0)) + max(c - vec3<f32>(1.0 + p.p0.z), vec3<f32>(0.0)) * p.p0.y, kLum);
      s += log2(clamp(p.p0.w - lit, p.p1.x, p.p0.w));
    }
  }
  let l = p.p0.w - exp2(s / 576.0);
  if (l >= p.p1.w) { return vec4<f32>(mix(p.p2.z, p.p2.w, saturate((l - p.p1.w) / (p.p2.x - p.p1.w))), 0.0, 0.0, 1.0); }
  return vec4<f32>(mix(p.p2.y, p.p2.z, saturate((l - p.p1.y) / (p.p1.z - p.p1.y))), 0.0, 0.0, 1.0);
}
// p1: source texel; a quarter size copy of what passes 1
@fragment fn bright(i: O) -> @location(0) vec4<f32> {
  var c = vec3<f32>(0.0);
  for (var k = 0; k < 4; k++) {
    let side = vec2<f32>(select(-1.0, 1.0, (k & 1) != 0), select(-1.0, 1.0, (k & 2) != 0));
    c += textureSampleLevel(src, ls, i.uv + p.p1.xy * side, 0.0).rgb;
  }
  return vec4<f32>(max(c * 0.25 * p.p0.x - vec3<f32>(1.0), vec3<f32>(0.0)), 1.0);
}
// p1: step; 9 gaussian taps in 5 bilinear reads
@fragment fn blur(i: O) -> @location(0) vec4<f32> {
  var c = textureSampleLevel(src, ls, i.uv, 0.0).rgb * 0.227027;
  c += (textureSampleLevel(src, ls, i.uv + p.p1.xy * 1.384615, 0.0).rgb + textureSampleLevel(src, ls, i.uv - p.p1.xy * 1.384615, 0.0).rgb) * 0.316216;
  c += (textureSampleLevel(src, ls, i.uv + p.p1.xy * 3.230769, 0.0).rgb + textureSampleLevel(src, ls, i.uv - p.p1.xy * 3.230769, 0.0).rgb) * 0.070270;
  return vec4<f32>(c, 1.0);
}
// p0: headroom, bloom strength; scene and glow times the exposure scale, clipped as Chaos Theory's
@fragment fn tone(i: O) -> @location(0) vec4<f32> {
  let s = textureLoad(src, vec2<i32>(i.pos.xy), 0);
  let c = s.rgb * p.p0.x + textureSampleLevel(bloom, ls, i.uv, 0.0).rgb * p.p0.y;
  return vec4<f32>(saturate(c * exposure_now()), s.a);
}
)";
constexpr float kHeadroom = 8, kBloomStrength = 0.6f;
// a level's Tonemapping settings (native's ToneMapPreset), glow start out of 255
struct ToneMapPreset {
  float glow_strength, glow_start, dark_scale, normal_scale, bright_scale, dark_end, normal_start, normal_end, bright_start, brightness_max,
    brightness_min, initial_scale;
};
// Double Agent's levels: Tonemapping and Tonemapping_NV; tone mapping off is every scale 1
constexpr ToneMapPreset kToneMap = {256, 16, 1.5f, 1.0f, 0.5f, 0.05f, 0.2f, 0.6f, 1.2f, 16, 0.01f, 1.5f};
constexpr ToneMapPreset kToneMapNv = {256, 16, 1.0f, 0.6f, 0.4f, 0.0f, 0.05f, 0.2f, 0.35f, 16, 0.01f, 2.4f};
constexpr ToneMapPreset kNoToneMap = {256, 16, 1, 1, 1, 0.05f, 0.2f, 0.6f, 1.2f, 16, 0.01f, 1};
constexpr uint32_t kBloomShrink = 4;
struct ResolveState {
  WGPUBindGroupLayout bindings = nullptr;
  WGPUPipelineLayout layout = nullptr;
  std::map<uint32_t, WGPURenderPipeline> pipelines;  // pass | 8 for an HDR scene target
  WGPUTexture copy = nullptr, bloom[2]{}, exposure = nullptr, dummy = nullptr;
  WGPUSampler sampler = nullptr;
  uint32_t width = 0, height = 0;
  uint64_t bytes = 0;
} resolve_state;
constexpr const char* kResolveEntries[4] = {"exposure", "bright", "blur", "tone"};
WGPURenderPipeline resolve_pipeline(uint32_t pass, WGPUTextureFormat format) {
  auto& state = resolve_state;
  const uint32_t key = pass | (format == hdr_format ? 8 : 0);
  if (const auto found = state.pipelines.find(key); found != state.pipelines.end()) { return found->second; }
  if (!state.bindings) {
    std::array<WGPUBindGroupLayoutEntry, 5> entries{};
    for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT; entries[n].binding = n; entries[n].visibility = WGPUShaderStage_Fragment; }
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    for (uint32_t n = 1; n < 4; ++n) { entries[n].texture.sampleType = WGPUTextureSampleType_Float; entries[n].texture.viewDimension = WGPUTextureViewDimension_2D; }
    entries[4].sampler.type = WGPUSamplerBindingType_Filtering;
    auto group = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; group.entryCount = entries.size(); group.entries = entries.data();
    state.bindings = wgpuDeviceCreateBindGroupLayout(device, &group);
    auto layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; layout.bindGroupLayoutCount = 1; layout.bindGroupLayouts = &state.bindings;
    state.layout = wgpuDeviceCreatePipelineLayout(device, &layout);
  }
  auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = text(kResolveWgsl);
  auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
  auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
  auto blend = WGPU_BLEND_STATE_INIT;
  blend.color.srcFactor = blend.alpha.srcFactor = WGPUBlendFactor_Constant;
  blend.color.dstFactor = blend.alpha.dstFactor = WGPUBlendFactor_OneMinusConstant;
  auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = format;
  if (pass == 0) { target.blend = &blend; }
  auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text(kResolveEntries[pass]); fragment.targetCount = 1; fragment.targets = &target;
  auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT; descriptor.layout = state.layout; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main");
  descriptor.fragment = &fragment; descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  auto pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
  wgpuShaderModuleRelease(shader);
  state.pipelines.emplace(key, pipeline);
  return pipeline;
}
void release_resolve_targets() {
  auto& state = resolve_state;
  for (auto* target : {&state.copy, &state.bloom[0], &state.bloom[1]}) { if (*target) { wgpuTextureDestroy(*target); wgpuTextureRelease(*target); *target = nullptr; } }
  texture_bytes -= state.bytes; state.bytes = 0;
  state.width = state.height = 0;
}
void release_resolve() {
  auto& state = resolve_state;
  release_resolve_targets();
  for (auto& [key, pipeline] : state.pipelines) { wgpuRenderPipelineRelease(pipeline); }
  state.pipelines.clear();
  for (auto* texture : {&state.exposure, &state.dummy}) { if (*texture) { wgpuTextureDestroy(*texture); wgpuTextureRelease(*texture); *texture = nullptr; } }
  if (state.layout) { wgpuPipelineLayoutRelease(state.layout); state.layout = nullptr; }
  if (state.bindings) { wgpuBindGroupLayoutRelease(state.bindings); state.bindings = nullptr; }
  if (state.sampler) { wgpuSamplerRelease(state.sampler); state.sampler = nullptr; }
}
WGPUTexture resolve_texture(uint32_t width, uint32_t height, WGPUTextureFormat format, WGPUTextureUsage usage) {
  auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT; descriptor.size = {width, height, 1}; descriptor.format = format; descriptor.usage = usage;
  return wgpuDeviceCreateTexture(device, &descriptor);
}
void resolve_pass(WGPUCommandEncoder encoder, uint32_t pass, WGPUTextureFormat format, WGPUTextureView output, std::array<float, 12> parameters,
                  WGPUTextureView source, WGPUTextureView bloom, WGPUTextureView exposure, float blend = 1) {
  auto& state = resolve_state;
  auto uniforms = buffer({reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)}, WGPUBufferUsage_Uniform);
  std::array<WGPUBindGroupEntry, 5> entries{};
  for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_ENTRY_INIT; entries[n].binding = n; }
  entries[0].buffer = uniforms; entries[0].size = sizeof(parameters);
  entries[1].textureView = source; entries[2].textureView = bloom; entries[3].textureView = exposure; entries[4].sampler = state.sampler;
  const auto pipeline = resolve_pipeline(pass, format);
  auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = state.bindings; group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
  auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
  auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; attachment.view = output; attachment.loadOp = WGPULoadOp_Load; attachment.storeOp = WGPUStoreOp_Store;
  auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &attachment;
  auto render = begin_pass(encoder, &descriptor);
  const WGPUColor constant{blend, blend, blend, blend};
  wgpuRenderPassEncoderSetBlendConstant(render, &constant);
  wgpuRenderPassEncoderSetPipeline(render, pipeline); wgpuRenderPassEncoderSetBindGroup(render, 0, group, 0, nullptr);
  wgpuRenderPassEncoderDraw(render, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(render); wgpuRenderPassEncoderRelease(render);
  wgpuBindGroupRelease(group); wgpuBufferRelease(uniforms);
}
// the frame is read: exposure, bloom and tone mapping take the scene back to 0..1
void resolve_hdr(uint32_t resource, std::span<const uint8_t> bytes) {
  const auto spec = packet<ResolvePacket>(bytes); exact(bytes, 0);
  auto& scene = texture(resource);
  if (!scene.hdr || !std::isfinite(spec.blend) || spec.blend < 0 || spec.blend > 1 || spec.tone > (ResolveTone | ResolveNightVision | ResolveFresh)) {
    throw std::runtime_error("invalid HDR resolve");
  }
  const ToneMapPreset& tm = !(spec.tone & ResolveTone) ? kNoToneMap : spec.tone & ResolveNightVision ? kToneMapNv : kToneMap;
  auto& state = resolve_state;
  end_batch_pass();
  readable(scene);
  const uint32_t bw = std::max(1u, scene.width / kBloomShrink), bh = std::max(1u, scene.height / kBloomShrink);
  auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
  const bool created = !state.sampler;
  if (created) {
    auto descriptor = WGPU_SAMPLER_DESCRIPTOR_INIT; descriptor.magFilter = descriptor.minFilter = WGPUFilterMode_Linear;
    state.sampler = wgpuDeviceCreateSampler(device, &descriptor);
    state.dummy = resolve_texture(1, 1, WGPUTextureFormat_RGBA16Float, WGPUTextureUsage_TextureBinding);
    state.exposure = resolve_texture(1, 1, WGPUTextureFormat_R16Float, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
  }
  // the scale starts at the preset's own; new memory can also hold NaN, which the blend would keep forever
  if (created || (spec.tone & ResolveFresh)) {
    const double initial = created && !(spec.tone & ResolveFresh) ? 1.0 : tm.initial_scale;
    auto view = wgpuTextureCreateView(state.exposure, nullptr);
    auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; attachment.view = view; attachment.loadOp = WGPULoadOp_Clear; attachment.storeOp = WGPUStoreOp_Store;
    attachment.clearValue = {initial, initial, initial, initial};
    auto descriptor_pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor_pass.colorAttachmentCount = 1; descriptor_pass.colorAttachments = &attachment;
    auto render = begin_pass(encoder, &descriptor_pass); wgpuRenderPassEncoderEnd(render); wgpuRenderPassEncoderRelease(render);
    wgpuTextureViewRelease(view);
  }
  if (state.width != scene.width || state.height != scene.height) {
    release_resolve_targets();
    state.copy = resolve_texture(scene.width, scene.height, hdr_format, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    for (auto& target : state.bloom) { target = resolve_texture(bw, bh, WGPUTextureFormat_RGBA16Float, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding); }
    state.width = scene.width; state.height = scene.height;
    state.bytes = uint64_t(scene.width) * scene.height * 8 + uint64_t(bw) * bh * 16;
    texture_bytes += state.bytes;
  }
  auto from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; from.texture = scene.handle;
  auto to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; to.texture = state.copy;
  const WGPUExtent3D extent{scene.width, scene.height, 1};
  wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
  auto copy = wgpuTextureCreateView(state.copy, nullptr), blank = wgpuTextureCreateView(state.dummy, nullptr), exposure = wgpuTextureCreateView(state.exposure, nullptr);
  std::array<WGPUTextureView, 2> bloom{wgpuTextureCreateView(state.bloom[0], nullptr), wgpuTextureCreateView(state.bloom[1], nullptr)};
  resolve_pass(encoder, 0, WGPUTextureFormat_R16Float, exposure, {kHeadroom, tm.glow_strength, tm.glow_start / 255, tm.brightness_max,
    tm.brightness_min, tm.dark_end, tm.normal_start, tm.normal_end, tm.bright_start, tm.dark_scale, tm.normal_scale, tm.bright_scale}, copy, blank, blank, spec.blend);
  resolve_pass(encoder, 1, WGPUTextureFormat_RGBA16Float, bloom[0], {kHeadroom, 0, 0, 0, 1.0f / scene.width, 1.0f / scene.height}, copy, blank, exposure);
  resolve_pass(encoder, 2, WGPUTextureFormat_RGBA16Float, bloom[1], {0, 0, 0, 0, 1.0f / bw, 0}, bloom[0], blank, blank);
  resolve_pass(encoder, 2, WGPUTextureFormat_RGBA16Float, bloom[0], {0, 0, 0, 0, 0, 1.0f / bh}, bloom[1], blank, blank);
  resolve_pass(encoder, 3, hdr_format, attachment_view(scene), {kHeadroom, kBloomStrength}, copy, bloom[0], exposure);
  if (scene.ms) { scene.seed = true; }
  if (!batch_encoder) { submit(encoder); }
  for (auto view : {copy, blank, exposure, bloom[0], bloom[1]}) { wgpuTextureViewRelease(view); }
}
// HDR output: the canvas in extended range sRGB, the presented frame's highlights stretched (native's hdr10 without
// PQ): gamma 2.2 to linear, inverse Reinhard on luminance so white reaches the headroom, sRGB encoded past 1
constexpr const char* kExpandWgsl = R"(
@group(0) @binding(0) var frame: texture_2d<f32>;
@group(0) @binding(1) var<uniform> most: vec4<f32>;
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
  let positions = array<vec2<f32>, 3>(vec2<f32>(-1,-1), vec2<f32>(3,-1), vec2<f32>(-1,3));
  return vec4<f32>(positions[index], 0, 1);
}
fn encode(x: vec3<f32>) -> vec3<f32> {
  return select(x * 12.92, 1.055 * pow(x, vec3<f32>(1.0 / 2.4)) - vec3<f32>(0.055), x > vec3<f32>(0.0031308));
}
@fragment fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  let c = pow(textureLoad(frame, vec2<i32>(position.xy), 0).rgb, vec3<f32>(2.2));
  let expanded = min(c / (1.0 - dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)) * (1.0 - 1.0 / most.x)), vec3<f32>(most.x));
  return vec4<f32>(encode(expanded), 1.0);
}
)";
WGPURenderPipeline expand_pipeline = nullptr;
bool surface_hdr = false, hdr_refused = false;
EM_JS(int, canvas_extended, (), {
  try { return Module.canvas.getContext('webgpu')?.getConfiguration?.()?.toneMapping?.mode === 'extended' ? 1 : 0; } catch (error) { return 0; }
});
void configure_surface(bool hdr) {
  auto configuration = WGPU_SURFACE_CONFIGURATION_INIT;
  auto management = WGPU_SURFACE_COLOR_MANAGEMENT_INIT;
  configuration.device = device; configuration.format = hdr ? WGPUTextureFormat_RGBA16Float : WGPUTextureFormat_RGBA8Unorm;
  if (hdr) { management.colorSpace = WGPUPredefinedColorSpace_SRGB; management.toneMappingMode = WGPUToneMappingMode_Extended; configuration.nextInChain = &management.chain; }
  configuration.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
  configuration.width = canvas_width; configuration.height = canvas_height;
  configuration.presentMode = WGPUPresentMode_Fifo; configuration.alphaMode = WGPUCompositeAlphaMode_Opaque;
  wgpuSurfaceConfigure(surface, &configuration);
  surface_hdr = hdr;
  if (hdr && !canvas_extended()) { hdr_refused = true; configure_surface(false); }
}
void expand_frame(WGPUCommandEncoder encoder, WGPUTexture canvas, float headroom) {
  if (!expand_pipeline) {
    auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = text(kExpandWgsl);
    auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
    auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = WGPUTextureFormat_RGBA16Float;
    auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text("fs_main"); fragment.targetCount = 1; fragment.targets = &target;
    auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main"); descriptor.fragment = &fragment;
    expand_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuShaderModuleRelease(shader);
  }
  const std::array<float, 4> most{std::clamp(headroom, 1.0f, 10.0f), 0, 0, 0};
  auto uniforms = buffer({reinterpret_cast<const uint8_t*>(most.data()), sizeof(most)}, WGPUBufferUsage_Uniform);
  auto frame = wgpuTextureCreateView(presentation_target.handle, nullptr), output = wgpuTextureCreateView(canvas, nullptr);
  std::array<WGPUBindGroupEntry, 2> entries{};
  for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_ENTRY_INIT; entries[n].binding = n; }
  entries[0].textureView = frame; entries[1].buffer = uniforms; entries[1].size = sizeof(most);
  auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = wgpuRenderPipelineGetBindGroupLayout(expand_pipeline, 0);
  group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
  auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
  wgpuBindGroupLayoutRelease(group_descriptor.layout);
  auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; attachment.view = output; attachment.loadOp = WGPULoadOp_Clear; attachment.storeOp = WGPUStoreOp_Store; attachment.clearValue = {0, 0, 0, 1};
  auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &attachment;
  auto render = begin_pass(encoder, &descriptor);
  wgpuRenderPassEncoderSetPipeline(render, expand_pipeline); wgpuRenderPassEncoderSetBindGroup(render, 0, group, 0, nullptr);
  wgpuRenderPassEncoderDraw(render, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(render); wgpuRenderPassEncoderRelease(render);
  wgpuBindGroupRelease(group); wgpuBufferRelease(uniforms); wgpuTextureViewRelease(frame); wgpuTextureViewRelease(output);
}
// native's ambient occlusion (src/gpu/d3d12.cpp's kAoHlsl in WGSL): occlusion or GTAO from the scene's depth into an
// rg16f pair (occlusion, view z), a blur across, then a blur down multiplied into the scene. Stencil bit 0 is the
// world; where it is clear (the sky) depth counts as the far plane
constexpr const char* kAoWgsl = R"(
struct A { proj: vec4<f32>, zp: vec4<f32>, vp: vec4<f32>, opt: vec4<f32>, fog: vec4<f32> }
@group(0) @binding(0) var<uniform> a: A;
@group(0) @binding(1) var depth: DEPTH_TYPE;
@group(0) @binding(2) var stencil: STENCIL_TYPE;
@group(0) @binding(3) var ao: texture_2d<f32>;
@vertex fn vs_main(@builtin(vertex_index) id: u32) -> @builtin(position) vec4<f32> {
  return vec4<f32>(vec2<f32>(f32((id << 1u) & 2u), f32(id & 2u)) * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
}
fn raw(p: vec2<i32>) -> f32 {
  if ((textureLoad(stencil, p, 0).r & 1u) != 0u) { return textureLoad(depth, p, 0); }
  return 1.0;
}
// view space position of a pixel. proj: 1 / scale xy, centre xy; zp: z = x / (depth - y); vp: viewport in pixels
fn view(px: vec2<f32>) -> vec3<f32> {
  let p = clamp(vec2<i32>(px), vec2<i32>(a.vp.xy), vec2<i32>(a.vp.xy + a.vp.zw) - vec2<i32>(1));
  let z = a.zp.x / (raw(p) - a.zp.y);
  let ndc = (vec2<f32>(p) + vec2<f32>(0.5) - a.vp.xy) / a.vp.zw * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0);
  return vec3<f32>((ndc - a.proj.zw) * a.proj.xy * z, z);
}
fn dither(i: i32) -> f32 {
  let values = array<f32, 16>(0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
  return values[i];
}
// normal from the nearer neighbour each way: no bend over depth edges
fn normal_at(px: vec2<f32>, c: vec3<f32>) -> vec3<f32> {
  let l = view(px - vec2<f32>(1.0, 0.0));
  let r = view(px + vec2<f32>(1.0, 0.0));
  let u = view(px - vec2<f32>(0.0, 1.0));
  let d = view(px + vec2<f32>(0.0, 1.0));
  let dx = select(c - l, r - c, abs(r.z - c.z) < abs(c.z - l.z));
  let dy = select(c - u, d - c, abs(d.z - c.z) < abs(c.z - u.z));
  return normalize(cross(dx, dy));
}
// opt: world radius, most pixels it may span; out: occlusion, view z
@fragment fn occlusion(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let c = view(pos.xy);
  let rad = a.opt.x * 0.5 * a.vp.zw / (a.proj.xy * c.z);
  let span = max(rad.x, rad.y);
  if (span < 1.0) { return vec4<f32>(1.0, c.z, 0.0, 0.0); }
  let n = normal_at(pos.xy, c);
  let k = min(1.0, a.opt.y / span);
  let R = a.opt.x * k;
  let cell = vec2<i32>(pos.xy) & vec2<i32>(3);
  let turn = dither(cell.y * 4 + cell.x) * (6.2831853 / 16.0);
  var occluded = 0.0;
  for (var i = 0; i < 16; i++) {
    let f = (f32(i) + 0.5) / 16.0;
    let t = f32(i) * 2.3999632 + turn;
    let v = (view(pos.xy + vec2<f32>(cos(t), sin(t)) * rad * (k * f)) - c) / R;
    let vv = dot(v, v);
    let w = max(1.0 - vv, 0.0);
    occluded += w * w * w * max((dot(v, n) - 0.05) / (vv + 0.01), 0.0);
  }
  return vec4<f32>(max(1.0 - occluded * 0.25, 0.0), c.z, 0.0, 0.0);
}
const kHalfPi = 1.5707963;
// GTAO (Jimenez 2016) at XeGTAO's high preset: 3 slices, 3 steps a side, squared step spread, falloff over the outer
// 61.5% of the radius, power 2.2
@fragment fn gtao(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let c = view(pos.xy);
  let rad = a.opt.x * 0.5 * a.vp.zw / (a.proj.xy * c.z);
  let span = max(rad.x, rad.y);
  if (span < 1.0) { return vec4<f32>(1.0, c.z, 0.0, 0.0); }
  let n = normal_at(pos.xy, c);
  let v = normalize(-c);
  let k = min(1.0, a.opt.y / span);
  let R = a.opt.x * k;
  let cell = vec2<i32>(pos.xy) & vec2<i32>(3);
  let slice_noise = dither(cell.y * 4 + cell.x) / 16.0;
  let step_noise = dither(cell.x * 4 + cell.y) / 16.0;
  var vis = 0.0;
  for (var s = 0; s < 3; s++) {
    let phi = (f32(s) + slice_noise) * (kHalfPi * 2.0 / 3.0);
    let dir = vec3<f32>(cos(phi), sin(phi), 0.0);
    // in pixels; screen y runs down, view y up
    let omega = vec2<f32>(dir.x, -dir.y) * rad * k;
    let ortho = dir - dot(dir, v) * v;
    let axis = normalize(cross(ortho, v));
    let pn = n - axis * dot(n, axis);
    let pn_len = max(length(pn), 1e-4);
    let cos_n = clamp(dot(pn, v) / pn_len, 0.0, 1.0);
    let an = sign(dot(ortho, pn)) * acos(cos_n);
    // x: the +omega side, y: the -omega side; lowest is flat along the slice's tangent
    let low = vec2<f32>(cos(an + kHalfPi), cos(an - kHalfPi));
    var hc = low;
    for (var j = 0; j < 3; j++) {
      let f = (f32(j) + step_noise) / 3.0;
      // at least 1.3 pixels out, off the centre pixel
      let off = omega * (f * f + 1.3 / length(omega));
      let d0 = view(pos.xy + off) - c;
      let d1 = view(pos.xy - off) - c;
      let len = max(vec2<f32>(length(d0), length(d1)), vec2<f32>(1e-4));
      let w = clamp((vec2<f32>(R) - len) / (R * 0.615), vec2<f32>(0.0), vec2<f32>(1.0));
      let sc = vec2<f32>(dot(d0, v), dot(d1, v)) / len;
      hc = max(hc, mix(low, sc, w));
    }
    let h0 = an + clamp(-acos(clamp(hc.y, -1.0, 1.0)) - an, -kHalfPi, kHalfPi);
    let h1 = an + clamp(acos(clamp(hc.x, -1.0, 1.0)) - an, -kHalfPi, kHalfPi);
    let sin_n = sin(an);
    let arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - an)) / 4.0;
    let arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - an)) / 4.0;
    vis += pn_len * (arc0 + arc1);
  }
  return vec4<f32>(pow(clamp(vis / 3.0, 0.0, 1.0), 2.2), c.z, 0.0, 0.0);
}
// opt.zw: step; 5 taps average the 4 pixel dither, depth edges left out. Out: occlusion, view z
fn smooth_ao(pos: vec2<f32>) -> vec2<f32> {
  let p = vec2<i32>(pos);
  let lo = vec2<i32>(a.vp.xy);
  let hi = vec2<i32>(a.vp.xy + a.vp.zw) - vec2<i32>(1);
  let z = textureLoad(ao, p, 0).y;
  var sum = 0.0;
  var count = 0.0;
  for (var i = -2; i <= 2; i++) {
    let s = textureLoad(ao, clamp(p + i * vec2<i32>(a.opt.zw), lo, hi), 0).xy;
    let w = select(1.0, 0.5, abs(i) == 2) * clamp(2.0 - abs(s.y - z) / (z * 0.02), 0.0, 1.0);
    sum += s.x * w;
    count += w;
  }
  return vec2<f32>(sum / count, z);
}
@fragment fn occlusion_blur(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  return vec4<f32>(smooth_ao(pos.xy), 0.0, 0.0);
}
// fog: the game's, already blended over the scene, fades the occlusion out as it thickens
@fragment fn occlusion_apply(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let s = smooth_ao(pos.xy);
  let fogged = a.fog.z * clamp(a.fog.x * s.y + a.fog.y, 0.0, 1.0);
  return vec4<f32>(vec3<f32>(mix(s.x, 1.0, fogged)), 1.0);
}
)";
struct OcclusionState {
  WGPUBindGroupLayout bindings[2]{};
  WGPUPipelineLayout layouts[2]{};
  std::map<uint32_t, WGPURenderPipeline> pipelines;  // pass | 4 multisampled depth | 8 4x scene | 16 HDR scene
  WGPUTexture targets[2]{}, dummy = nullptr;
  uint32_t width = 0, height = 0;
} occlusion_state;
constexpr const char* kAoEntries[4] = {"occlusion", "gtao", "occlusion_blur", "occlusion_apply"};
WGPURenderPipeline occlusion_pipeline(uint32_t pass, bool depthMs, bool sceneMs, bool hdr) {
  auto& state = occlusion_state;
  const uint32_t key = pass | (depthMs ? 4 : 0) | (pass == 3 && sceneMs ? 8 : 0) | (pass == 3 && hdr ? 16 : 0);
  if (const auto found = state.pipelines.find(key); found != state.pipelines.end()) { return found->second; }
  if (!state.bindings[depthMs]) {
    std::array<WGPUBindGroupLayoutEntry, 4> entries{};
    for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT; entries[n].binding = n; entries[n].visibility = WGPUShaderStage_Fragment; }
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].texture.sampleType = WGPUTextureSampleType_Depth; entries[1].texture.viewDimension = WGPUTextureViewDimension_2D; entries[1].texture.multisampled = depthMs;
    entries[2].texture.sampleType = WGPUTextureSampleType_Uint; entries[2].texture.viewDimension = WGPUTextureViewDimension_2D; entries[2].texture.multisampled = depthMs;
    entries[3].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat; entries[3].texture.viewDimension = WGPUTextureViewDimension_2D;
    auto group = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; group.entryCount = entries.size(); group.entries = entries.data();
    state.bindings[depthMs] = wgpuDeviceCreateBindGroupLayout(device, &group);
    auto layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; layout.bindGroupLayoutCount = 1; layout.bindGroupLayouts = &state.bindings[depthMs];
    state.layouts[depthMs] = wgpuDeviceCreatePipelineLayout(device, &layout);
  }
  std::string code = kAoWgsl;
  for (const auto& [marker, type] : {std::pair<std::string, std::string>{"DEPTH_TYPE", depthMs ? "texture_depth_multisampled_2d" : "texture_depth_2d"},
      std::pair<std::string, std::string>{"STENCIL_TYPE", depthMs ? "texture_multisampled_2d<u32>" : "texture_2d<u32>"}}) {
    code.replace(code.find(marker), marker.size(), type);
  }
  auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = {code.data(), code.size()};
  auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
  auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
  // the apply multiplies the scene's colour and keeps the alpha the game's blends read
  auto blend = WGPU_BLEND_STATE_INIT;
  blend.color.srcFactor = blend.alpha.srcFactor = WGPUBlendFactor_Zero;
  blend.color.dstFactor = WGPUBlendFactor_Src; blend.alpha.dstFactor = WGPUBlendFactor_One;
  auto target = WGPU_COLOR_TARGET_STATE_INIT;
  target.format = pass == 3 ? (hdr ? hdr_format : WGPUTextureFormat_RGBA8Unorm) : WGPUTextureFormat_RG16Float;
  if (pass == 3) { target.blend = &blend; }
  auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text(kAoEntries[pass]); fragment.targetCount = 1; fragment.targets = &target;
  auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT; descriptor.layout = state.layouts[depthMs]; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main");
  descriptor.fragment = &fragment; descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  if (pass == 3 && sceneMs) { descriptor.multisample.count = 4; }
  auto pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
  wgpuShaderModuleRelease(shader);
  state.pipelines.emplace(key, pipeline);
  return pipeline;
}
void release_occlusion_targets() {
  auto& state = occlusion_state;
  for (auto& target : state.targets) { if (target) { wgpuTextureDestroy(target); wgpuTextureRelease(target); target = nullptr; } }
  if (state.width) { texture_bytes -= uint64_t(state.width) * state.height * 8; }
  state.width = state.height = 0;
}
void release_occlusion() {
  auto& state = occlusion_state;
  release_occlusion_targets();
  for (auto& [key, pipeline] : state.pipelines) { wgpuRenderPipelineRelease(pipeline); }
  state.pipelines.clear();
  if (state.dummy) { wgpuTextureDestroy(state.dummy); wgpuTextureRelease(state.dummy); state.dummy = nullptr; }
  for (auto& layout : state.layouts) { if (layout) { wgpuPipelineLayoutRelease(layout); layout = nullptr; } }
  for (auto& group : state.bindings) { if (group) { wgpuBindGroupLayoutRelease(group); group = nullptr; } }
}
void ambient_occlusion(uint32_t resource, std::span<const uint8_t> bytes) {
  const auto spec = packet<OcclusionPacket>(bytes); exact(bytes, 0);
  auto& scene = texture(resource); auto& depth = texture(spec.depth);
  const float* vp = spec.constants + 8;
  if (scene.depth || !depth.depth || scene.width != depth.width || scene.height != depth.height || bool(scene.ms) != bool(depth.ms) || spec.gtao > 1 ||
      !std::all_of(spec.constants, spec.constants + 20, [](float value) { return std::isfinite(value); }) || vp[0] < 0 || vp[1] < 0 || !(vp[2] >= 1) || !(vp[3] >= 1) ||
      vp[0] + vp[2] > float(scene.width) || vp[1] + vp[3] > float(scene.height)) {
    throw std::runtime_error("invalid ambient occlusion request");
  }
  auto& state = occlusion_state;
  end_batch_pass();
  if (state.width != scene.width || state.height != scene.height) {
    release_occlusion_targets();
    for (auto& target : state.targets) { target = post_texture(scene.width, scene.height, WGPUTextureFormat_RG16Float); }
    state.width = scene.width; state.height = scene.height;
    texture_bytes += uint64_t(scene.width) * scene.height * 8;
  }
  if (!state.dummy) { state.dummy = post_texture(1, 1, WGPUTextureFormat_RG16Float); }
  const bool multisampled = bool(depth.ms);
  auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
  if (multisampled) { seed(encoder, scene); }
  auto aspect = [&](WGPUTextureAspect part) {
    auto descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT; descriptor.aspect = part;
    return wgpuTextureCreateView(multisampled ? depth.ms : depth.handle, &descriptor);
  };
  auto depthView = aspect(WGPUTextureAspect_DepthOnly), stencilView = aspect(WGPUTextureAspect_StencilOnly);
  std::array<WGPUTextureView, 2> views{wgpuTextureCreateView(state.targets[0], nullptr), wgpuTextureCreateView(state.targets[1], nullptr)};
  auto blank = wgpuTextureCreateView(state.dummy, nullptr);
  const auto pass = [&](uint32_t entry, WGPUTextureView input, WGPUTextureView output, WGPUTextureView resolve, float stepX, float stepY) {
    std::array<float, 20> constants;
    std::copy(spec.constants, spec.constants + 20, constants.begin());
    constants[14] = stepX; constants[15] = stepY;
    auto uniforms = buffer({reinterpret_cast<const uint8_t*>(constants.data()), sizeof(constants)}, WGPUBufferUsage_Uniform);
    const auto pipeline = occlusion_pipeline(entry, multisampled, bool(scene.ms), scene.hdr);
    std::array<WGPUBindGroupEntry, 4> entries{};
    for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_ENTRY_INIT; entries[n].binding = n; }
    entries[0].buffer = uniforms; entries[0].size = sizeof(constants);
    entries[1].textureView = depthView; entries[2].textureView = stencilView; entries[3].textureView = input;
    auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = state.bindings[multisampled]; group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
    auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
    auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; attachment.view = output; attachment.resolveTarget = resolve;
    attachment.loadOp = WGPULoadOp_Load; attachment.storeOp = WGPUStoreOp_Store;
    auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &attachment;
    auto render = begin_pass(encoder, &descriptor);
    wgpuRenderPassEncoderSetPipeline(render, pipeline); wgpuRenderPassEncoderSetBindGroup(render, 0, group, 0, nullptr);
    wgpuRenderPassEncoderSetScissorRect(render, uint32_t(vp[0]), uint32_t(vp[1]), uint32_t(vp[2]), uint32_t(vp[3]));
    wgpuRenderPassEncoderDraw(render, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(render); wgpuRenderPassEncoderRelease(render);
    wgpuBindGroupRelease(group); wgpuBufferRelease(uniforms);
  };
  pass(spec.gtao ? 1 : 0, blank, views[0], nullptr, 0, 0);
  pass(2, views[0], views[1], nullptr, 1, 0);
  pass(3, views[1], scene.ms ? scene.msView : attachment_view(scene), scene.ms ? attachment_view(scene) : nullptr, 0, 1);
  if (!batch_encoder) { submit(encoder); }
  for (auto view : {depthView, stencilView, views[0], views[1], blank}) { wgpuTextureViewRelease(view); }
}
void clear(uint32_t resource, std::span<const uint8_t> bytes) {
      const auto spec = packet<ClearPacket>(bytes); exact(bytes, uint64_t(spec.rect_count) * 16);
      if (!spec.flags || spec.flags > 7 || spec.rect_count > 4096 || spec.stencil > 255 || !std::isfinite(spec.depth) || spec.depth < 0 || spec.depth > 1 ||
          std::any_of(std::begin(spec.color), std::end(spec.color), [](float value) { return !std::isfinite(value) || value < 0 || value > 1; })) { throw std::runtime_error("invalid clear state"); }
      auto* target = spec.flags & 1 ? &texture(resource) : nullptr;
      auto* depth_target = spec.flags & 6 ? &texture(spec.depth_target) : nullptr;
      if ((target && (target->depth || target->cube || target->volumeDepth > 1)) || (depth_target && !depth_target->depth) ||
          (target && depth_target && (target->width != depth_target->width || target->height != depth_target->height))) { throw std::runtime_error("invalid clear attachment"); }
      auto color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; auto depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
      auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
      const bool multisampled = (target && target->ms) || (depth_target && depth_target->ms);
      if ((target && depth_target && bool(target->ms) != bool(depth_target->ms))) { throw std::runtime_error("invalid clear attachment samples"); }
      if (target) {
        color.view = wgpuTextureCreateView(target->ms ? target->ms : target->handle, nullptr); color.loadOp = spec.rect_count ? WGPULoadOp_Load : WGPULoadOp_Clear;
        if (target->ms) { color.resolveTarget = attachment_view(*target); }
        color.storeOp = WGPUStoreOp_Store; color.clearValue = {spec.color[0], spec.color[1], spec.color[2], spec.color[3]};
        descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &color;
      }
      if (depth_target) {
        depth.view = wgpuTextureCreateView(depth_target->ms ? depth_target->ms : depth_target->handle, nullptr);
        if (depth_target->ms) { depth_target->stale = true; }
        depth.depthLoadOp = !spec.rect_count && (spec.flags & 2) ? WGPULoadOp_Clear : WGPULoadOp_Load;
        depth.stencilLoadOp = !spec.rect_count && (spec.flags & 4) ? WGPULoadOp_Clear : WGPULoadOp_Load;
        depth.depthStoreOp = WGPUStoreOp_Store; depth.stencilStoreOp = WGPUStoreOp_Store;
        depth.depthClearValue = spec.depth; depth.stencilClearValue = spec.stencil; descriptor.depthStencilAttachment = &depth;
      }
      WGPUBuffer uniforms = nullptr; WGPUBindGroup group = nullptr;
      WGPURenderPipeline pipeline = nullptr;
      if (spec.rect_count) {
        pipeline = partial_clear_pipeline(spec.flags + (multisampled ? 9 : 0) + (target && target->hdr ? 18 : 0));
        std::array<float, 8> parameters{spec.color[0], spec.color[1], spec.color[2], spec.color[3], spec.depth};
        uniforms = buffer({reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)}, WGPUBufferUsage_Uniform);
        auto entry = WGPU_BIND_GROUP_ENTRY_INIT; entry.binding = 0; entry.buffer = uniforms; entry.size = sizeof(parameters);
        auto descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; descriptor.layout = partial_clear_bindings; descriptor.entryCount = 1; descriptor.entries = &entry;
        group = wgpuDeviceCreateBindGroup(device, &descriptor);
      }
      end_batch_pass();
      auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
      for (auto* item : {target, depth_target}) {
        if (!item) { continue; }
        if (spec.rect_count) { seed(encoder, *item); } else { item->seed = false; }
      }
      auto pass = begin_pass(encoder, &descriptor);
      if (spec.rect_count) {
        wgpuRenderPassEncoderSetPipeline(pass, pipeline); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        wgpuRenderPassEncoderSetStencilReference(pass, spec.stencil);
        const auto& dimensions = target ? *target : *depth_target;
        for (uint32_t n = 0; n < spec.rect_count; ++n) {
          std::array<int32_t, 4> rectangle{};
          std::memcpy(rectangle.data(), bytes.data() + n * 16, 16);
          const uint32_t left = std::clamp<int64_t>(rectangle[0], 0, dimensions.width), right = std::clamp<int64_t>(rectangle[2], 0, dimensions.width);
          const uint32_t top = std::clamp<int64_t>(rectangle[1], 0, dimensions.height), bottom = std::clamp<int64_t>(rectangle[3], 0, dimensions.height);
          if (left >= right || top >= bottom) { continue; }
          wgpuRenderPassEncoderSetScissorRect(pass, left, top, right - left, bottom - top); wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        }
      }
      wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
      if (!batch_encoder) { submit(encoder); }
      if (group) { wgpuBindGroupRelease(group); } if (uniforms) { wgpuBufferRelease(uniforms); }
      if (color.view) { wgpuTextureViewRelease(color.view); } if (depth.view) { wgpuTextureViewRelease(depth.view); }
}
WGPUBindGroupLayout blit_bindings[2];
WGPUPipelineLayout blit_layouts[2];
WGPURenderPipeline blit_pipelines[5];
// 0 colour, 1 colour with alpha forced to 1, 2 depth, 3 and 4 as 0 and 1 into an HDR scene target
WGPURenderPipeline blit_pipeline(uint32_t kind) {
  if (blit_pipelines[kind]) { return blit_pipelines[kind]; }
  const bool depth = kind == 2;
  if (!blit_layouts[depth]) {
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT; entries[n].binding = n; entries[n].visibility = WGPUShaderStage_Fragment; }
    entries[0].texture.sampleType = depth ? WGPUTextureSampleType_Depth : WGPUTextureSampleType_UnfilterableFloat;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    auto group = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; group.entryCount = entries.size(); group.entries = entries.data();
    blit_bindings[depth] = wgpuDeviceCreateBindGroupLayout(device, &group);
    auto layout_descriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; layout_descriptor.bindGroupLayoutCount = 1; layout_descriptor.bindGroupLayouts = &blit_bindings[depth];
    blit_layouts[depth] = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
  }
  std::string code = R"(
struct Parameters { source: vec4<f32>, destination: vec4<f32>, taps: u32, opaque: u32, scale: f32, padding: u32 }
@group(0) @binding(1) var<uniform> parameters: Parameters;
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
  let positions = array<vec2<f32>, 3>(vec2<f32>(-1,-1), vec2<f32>(3,-1), vec2<f32>(-1,3));
  return vec4<f32>(positions[index], 0, 1);
}
fn source_at(position: vec2<f32>) -> vec2<f32> {
  return parameters.source.xy + (position - parameters.destination.xy) * parameters.source.zw / parameters.destination.zw;
}
)";
  if (depth) {
    code += R"(
@group(0) @binding(0) var image: texture_depth_2d;
@fragment fn fs_main(@builtin(position) position: vec4<f32>) -> @builtin(frag_depth) f32 {
  let last = vec2<i32>(textureDimensions(image)) - vec2<i32>(1);
  return textureLoad(image, clamp(vec2<i32>(floor(source_at(position.xy))), vec2<i32>(0), last), 0);
}
)";
  } else {
    code += R"(
@group(0) @binding(0) var image: texture_2d<f32>;
fn texel(at: vec2<i32>) -> vec4<f32> {
  return textureLoad(image, clamp(at, vec2<i32>(0), vec2<i32>(textureDimensions(image)) - vec2<i32>(1)), 0);
}
@fragment fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  var color: vec4<f32>;
  if (parameters.taps == 0u) {
    let at = source_at(position.xy) - vec2<f32>(0.5);
    let corner = floor(at);
    let weight = at - corner;
    let a = vec2<i32>(corner);
    color = mix(mix(texel(a), texel(a + vec2<i32>(1, 0)), weight.x), mix(texel(a + vec2<i32>(0, 1)), texel(a + vec2<i32>(1, 1)), weight.x), weight.y);
  } else {
    let span = parameters.source.zw / parameters.destination.zw;
    let start = source_at(position.xy - vec2<f32>(0.5));
    let count = parameters.taps;
    for (var y = 0u; y < count; y++) {
      for (var x = 0u; x < count; x++) {
        let offset = (vec2<f32>(f32(x), f32(y)) + vec2<f32>(0.5)) * span / f32(count);
        color += texel(vec2<i32>(floor(start + offset)));
      }
    }
    color /= f32(count * count);
  }
  color = vec4<f32>(color.rgb * parameters.scale, color.a);
  if (parameters.opaque != 0u) { color.a = 1.0; }
  return color;
}
)";
  }
  auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = {code.data(), code.size()};
  auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
  auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
  auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  descriptor.layout = blit_layouts[depth]; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main");
  descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  auto color = WGPU_COLOR_TARGET_STATE_INIT; color.format = kind >= 3 ? hdr_format : WGPUTextureFormat_RGBA8Unorm;
  auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text("fs_main");
  if (!depth) { fragment.targetCount = 1; fragment.targets = &color; }
  descriptor.fragment = &fragment;
  auto stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
  if (depth) {
    stencil.format = WGPUTextureFormat_Depth24PlusStencil8; stencil.depthWriteEnabled = WGPUOptionalBool_True; stencil.depthCompare = WGPUCompareFunction_Always;
    stencil.stencilReadMask = stencil.stencilWriteMask = 0;
    descriptor.depthStencil = &stencil;
  }
  blit_pipelines[kind] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
  wgpuShaderModuleRelease(shader);
  return blit_pipelines[kind];
}
void release_blits() {
  for (auto& pipeline : blit_pipelines) { if (pipeline) { wgpuRenderPipelineRelease(pipeline); pipeline = nullptr; } }
  for (auto& layout : blit_layouts) { if (layout) { wgpuPipelineLayoutRelease(layout); layout = nullptr; } }
  for (auto& group : blit_bindings) { if (group) { wgpuBindGroupLayoutRelease(group); group = nullptr; } }
}
void blit(uint32_t resource, std::span<const uint8_t> bytes) {
  const auto spec = packet<BlitPacket>(bytes); exact(bytes, 0);
  auto& source = texture(spec.source); auto& target = texture(resource);
  const auto inside = [](const float* rect, const Texture& texture) {
    return std::all_of(rect, rect + 4, [](float value) { return std::isfinite(value) && value >= 0; }) && rect[2] > 0 && rect[3] > 0 &&
      rect[0] + rect[2] <= float(texture.width) && rect[1] + rect[3] <= float(texture.height);
  };
  if (spec.source == resource || source.depth != target.depth || source.cube || target.cube || source.volumeDepth > 1 || target.volumeDepth > 1 ||
      spec.taps > 16 || spec.force_opaque > 1 || !inside(spec.source_rect, source) || !inside(spec.destination_rect, target) || !(spec.scale > 0 && spec.scale <= 8)) {
    throw std::runtime_error("invalid texture blit");
  }
  end_batch_pass();
  readable(source);
  if (target.ms) { target.seed = true; target.stale = false; }
  const uint32_t kind = target.depth ? 2 : spec.force_opaque + (target.hdr ? 3 : 0);
  const auto pipeline = blit_pipeline(kind);
  const std::array<uint32_t, 12> parameters{std::bit_cast<uint32_t>(spec.source_rect[0]), std::bit_cast<uint32_t>(spec.source_rect[1]),
    std::bit_cast<uint32_t>(spec.source_rect[2]), std::bit_cast<uint32_t>(spec.source_rect[3]), std::bit_cast<uint32_t>(spec.destination_rect[0]),
    std::bit_cast<uint32_t>(spec.destination_rect[1]), std::bit_cast<uint32_t>(spec.destination_rect[2]), std::bit_cast<uint32_t>(spec.destination_rect[3]),
    spec.taps, spec.force_opaque, std::bit_cast<uint32_t>(spec.scale), 0};
  auto uniforms = buffer({reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)}, WGPUBufferUsage_Uniform);
  auto view_descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
  if (source.depth) { view_descriptor.aspect = WGPUTextureAspect_DepthOnly; }
  auto input = wgpuTextureCreateView(source.handle, &view_descriptor);
  std::array<WGPUBindGroupEntry, 2> entries{};
  entries[0] = WGPU_BIND_GROUP_ENTRY_INIT; entries[0].binding = 0; entries[0].textureView = input;
  entries[1] = WGPU_BIND_GROUP_ENTRY_INIT; entries[1].binding = 1; entries[1].buffer = uniforms; entries[1].size = sizeof(parameters);
  auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = blit_bindings[target.depth]; group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
  auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
  auto output = wgpuTextureCreateView(target.handle, nullptr);
  auto color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; auto depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
  auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  if (target.depth) {
    depth.view = output; depth.depthLoadOp = depth.stencilLoadOp = WGPULoadOp_Load; depth.depthStoreOp = depth.stencilStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 0; depth.stencilClearValue = 0;
    descriptor.depthStencilAttachment = &depth;
  } else {
    color.view = output; color.loadOp = WGPULoadOp_Load; color.storeOp = WGPUStoreOp_Store;
    descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &color;
  }
  auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
  auto pass = begin_pass(encoder, &descriptor);
  const auto* rect = spec.destination_rect;
  const uint32_t x = uint32_t(rect[0]), y = uint32_t(rect[1]);
  const uint32_t width = std::min(uint32_t(std::ceil(rect[0] + rect[2])), target.width) - x, height = std::min(uint32_t(std::ceil(rect[1] + rect[3])), target.height) - y;
  wgpuRenderPassEncoderSetScissorRect(pass, x, y, width, height);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
  if (!batch_encoder) { submit(encoder); }
  wgpuTextureViewRelease(output); wgpuTextureViewRelease(input); wgpuBindGroupRelease(group); wgpuBufferRelease(uniforms);
}
void copy(uint32_t resource, std::span<const uint8_t> bytes) {
      auto copy = packet<CopyPacket>(bytes); exact(bytes, 0);
      auto& source = texture(copy.source); auto& target = texture(resource);
      if (target.ms) { target.seed = true; }
      if (source.depth || target.depth || source.cube || target.cube || source.volumeDepth > 1 || target.volumeDepth > 1 || copy.force_opaque > 1 || !copy.width || !copy.height || uint64_t(copy.source_x) + copy.width > source.width || uint64_t(copy.source_y) + copy.height > source.height || uint64_t(copy.destination_x) + copy.width > target.width || uint64_t(copy.destination_y) + copy.height > target.height || copy.source == resource || source.hdr != target.hdr) { throw std::runtime_error("invalid texture copy region, format or alias"); }
      auto from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; from.texture = source.handle; from.origin = {copy.source_x, copy.source_y, 0}; auto to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; to.texture = target.handle; to.origin = {copy.destination_x, copy.destination_y, 0};
      WGPUExtent3D extent{copy.width, copy.height, 1};
      end_batch_pass();
      auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr); wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
      if (copy.force_opaque) { force_opaque_alpha(encoder, target, copy); }
      if (!batch_encoder) { submit(encoder); }
}
void render_batch(uint32_t resource, std::span<const uint8_t> bytes, bool mixed, std::span<const uint8_t> pending = {}, uint32_t uploadCount = 0) {
  if (resource) { throw std::runtime_error("draw batch cannot name a target"); }
  const uint32_t count = packet<uint32_t>(bytes);
  if ((!count && !uploadCount) || count > 256 || uploadCount > 256 || count + uploadCount > 256) { throw std::runtime_error("invalid draw batch count"); }
  DrawUploads uploads;
  uploads.prepare(bytes, count, mixed);
  for (uint32_t n = 0; n < uploadCount; ++n) {
    const uint32_t target = packet<uint32_t>(pending), length = packet<uint32_t>(pending);
    if (length > pending.size()) { throw std::runtime_error("truncated batched buffer upload"); }
    upload_buffer(target, pending.first(length)); pending = pending.subspan(length);
  }
  exact(pending, 0); created_resource = 0;
  if (!count) { return; }
  auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
  batch_encoder = encoder;
  try {
    for (uint32_t n = 0; n < count; ++n) {
      const auto opcode = mixed ? static_cast<Opcode>(packet<uint32_t>(bytes)) : Opcode::Draw;
      const uint32_t target = packet<uint32_t>(bytes), size = packet<uint32_t>(bytes);
      if (size > bytes.size()) { throw std::runtime_error("truncated batched draw"); }
      try {
        if (opcode == Opcode::Draw) { draw(target, bytes.first(size), &uploads, n); }
        else if (opcode == Opcode::Clear) { clear(target, bytes.first(size)); }
        else if (opcode == Opcode::Copy) { copy(target, bytes.first(size)); }
        else { throw std::runtime_error("unsupported staged render command"); }
      }
      catch (const std::exception& error) { throw std::runtime_error("batched draw " + std::to_string(n) + " target=" + std::to_string(target) + ": " + error.what()); }
      bytes = bytes.subspan(size);
    }
    exact(bytes, 0);
  } catch (...) {
    end_batch_pass();
    batch_encoder = nullptr;
    wgpuCommandEncoderRelease(encoder);
    throw;
  }
  end_batch_pass();
  batch_encoder = nullptr;
  submit(encoder);

}
void release_resources() {
  async_reads.clear(); collect_waiting = 0; present_waiting = false;
  for (const auto& [key, item] : sampler_cache) { wgpuSamplerRelease(item.handle); }
  sampler_cache.clear(); sampler_sequence = 0;
  for (auto& pipeline : partial_clear_pipelines) { if (pipeline) { wgpuRenderPipelineRelease(pipeline); pipeline = nullptr; } }
  if (partial_clear_layout) { wgpuPipelineLayoutRelease(partial_clear_layout); partial_clear_layout = nullptr; }
  if (partial_clear_bindings) { wgpuBindGroupLayoutRelease(partial_clear_bindings); partial_clear_bindings = nullptr; }
  if (depth_upload_pipeline) { wgpuRenderPipelineRelease(depth_upload_pipeline); depth_upload_pipeline = nullptr; }
  release_blits();
  release_msaa();
  release_smaa();
  release_soft();
  release_resolve();
  release_occlusion();
  if (expand_pipeline) { wgpuRenderPipelineRelease(expand_pipeline); expand_pipeline = nullptr; }
  if (depth_upload_layout) { wgpuPipelineLayoutRelease(depth_upload_layout); depth_upload_layout = nullptr; }
  if (depth_upload_bindings) { wgpuBindGroupLayoutRelease(depth_upload_bindings); depth_upload_bindings = nullptr; }
  if (depth_readback_pipeline) { wgpuRenderPipelineRelease(depth_readback_pipeline); depth_readback_pipeline = nullptr; }
  if (depth_readback_layout) { wgpuPipelineLayoutRelease(depth_readback_layout); depth_readback_layout = nullptr; }
  if (depth_readback_bindings) { wgpuBindGroupLayoutRelease(depth_readback_bindings); depth_readback_bindings = nullptr; }
  release_presentation_target();
  if (gamma_texture) { wgpuTextureDestroy(gamma_texture); wgpuTextureRelease(gamma_texture); gamma_texture = nullptr; }
  if (presentation_pipeline) { wgpuRenderPipelineRelease(presentation_pipeline); presentation_pipeline = nullptr; }
  if (presentation_linear_pipeline) { wgpuRenderPipelineRelease(presentation_linear_pipeline); presentation_linear_pipeline = nullptr; }
  if (presentation_layout) { wgpuPipelineLayoutRelease(presentation_layout); presentation_layout = nullptr; }
  if (presentation_bindings) { wgpuBindGroupLayoutRelease(presentation_bindings); presentation_bindings = nullptr; }
  for (auto& entry : textures) { release_views(entry.second); wgpuTextureDestroy(entry.second.handle); wgpuTextureRelease(entry.second.handle); }
  textures.clear(); texture_bytes = 0;
  for (auto& entry : pipelines) { wgpuRenderPipelineRelease(entry.second.handle); }
  pipelines.clear();
  for (const auto& entry : resident_buffers) { wgpuBufferDestroy(entry.second.handle); wgpuBufferRelease(entry.second.handle); }
  resident_buffers.clear(); resident_buffer_bytes = 0;
}
std::pair<WGPUBindGroupLayout, WGPUPipelineLayout> create_bindings(uint32_t cubeMask, uint32_t shadowMask = 0, uint32_t volumeMask = 0) {
  std::array<WGPUBindGroupLayoutEntry, 9> entries{};
  for (uint32_t n = 0; n < entries.size(); ++n) {
    entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[n].binding = n;
    entries[n].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    if (n == 0) { entries[n].buffer.type = WGPUBufferBindingType_Uniform; }
    else if (n % 2 == 1) { entries[n].texture.sampleType = shadowMask & (1u << ((n - 1) / 2)) ? WGPUTextureSampleType_Depth : WGPUTextureSampleType_Float; entries[n].texture.viewDimension = cubeMask & (1u << ((n - 1) / 2)) ? WGPUTextureViewDimension_Cube : volumeMask & (1u << ((n - 1) / 2)) ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D; }
    else { entries[n].sampler.type = WGPUSamplerBindingType_Filtering; }
  }
  auto descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  descriptor.entryCount = entries.size(); descriptor.entries = entries.data();
  auto bindings = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
  auto pipeline = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  pipeline.bindGroupLayoutCount = 1; pipeline.bindGroupLayouts = &bindings;
  return {bindings, wgpuDeviceCreatePipelineLayout(device, &pipeline)};
}
void initialize_bindings() {
  const auto created = create_bindings(0);
  bindings = created.first; layout = created.second;
  auto sampling = WGPU_SAMPLER_DESCRIPTOR_INIT;
  sampler = wgpuDeviceCreateSampler(device, &sampling);
}
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void webgpu_start() {
  register_canvas();
  instance = wgpuCreateInstance(nullptr);
  auto source = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
  source.selector = text("#canvas");
  auto descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
  descriptor.nextInChain = &source.chain;
  surface = wgpuInstanceCreateSurface(instance, &descriptor);
  auto info = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  info.mode = WGPUCallbackMode_AllowSpontaneous;
  info.callback = [](WGPURequestAdapterStatus status, WGPUAdapter value, WGPUStringView message, void*, void*) {
    if (status != WGPURequestAdapterStatus_Success) { ready(1, string(message).c_str()); return; }
    adapter = value;
    auto details = WGPU_ADAPTER_INFO_INIT; auto limits = WGPU_LIMITS_INIT; auto features = WGPU_SUPPORTED_FEATURES_INIT;
    wgpuAdapterGetInfo(adapter, &details); wgpuAdapterGetLimits(adapter, &limits); wgpuAdapterGetFeatures(adapter, &features);
    adapter_details(string(details.vendor).c_str(), string(details.architecture).c_str(), string(details.description).c_str(), reinterpret_cast<const uint32_t*>(features.features), features.featureCount, limits.maxTextureDimension2D, limits.maxVertexAttributes, limits.maxVertexBufferArrayStride, limits.maxUniformBufferBindingSize, (wgpuAdapterHasFeature(adapter, WGPUFeatureName_TextureCompressionBC) ? 1 : 0) | (wgpuAdapterHasFeature(adapter, WGPUFeatureName_TextureCompressionETC2) ? 2 : 0) | (wgpuAdapterHasFeature(adapter, WGPUFeatureName_TextureCompressionASTC) ? 4 : 0));
    wgpuAdapterInfoFreeMembers(details); wgpuSupportedFeaturesFreeMembers(features);
    auto descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
    descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    descriptor.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message, void*, void*) {
      if (!closed) { closed = true; active = false; lost(string(message).c_str()); }
    };
    descriptor.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType, WGPUStringView message, void*, void*) { closed = true; active = false; uncaptured(string(message).c_str()); };
    auto info = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = [](WGPURequestDeviceStatus status, WGPUDevice value, WGPUStringView message, void*, void*) {
      if (status != WGPURequestDeviceStatus_Success) { ready(1, string(message).c_str()); return; }
      device = value; queue = wgpuDeviceGetQueue(device); initialize_bindings(); ready(0, "");
    };
    wgpuAdapterRequestDevice(adapter, &descriptor, info);
  };
  auto options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
  options.compatibleSurface = surface;
  wgpuInstanceRequestAdapter(instance, &options, info);
}
#ifdef WEBGPU_SERVICE_TESTING
EMSCRIPTEN_KEEPALIVE void webgpu_test_loss() { wgpuDeviceDestroy(device); }
#endif
EMSCRIPTEN_KEEPALIVE const uint32_t* webgpu_frame_stats() { return presented_stats.data(); }
EMSCRIPTEN_KEEPALIVE uint32_t webgpu_resources() { return textures.size() + pipelines.size() + resident_buffers.size() + sampler_cache.size() + cached_views + (gamma_texture ? 1 : 0) + (presentation_pipeline ? 1 : 0) + (presentation_linear_pipeline ? 1 : 0) + (presentation_target.handle ? 1 : 0) + (depth_upload_pipeline ? 1 : 0) + (depth_readback_pipeline ? 1 : 0) + std::count_if(partial_clear_pipelines.begin(), partial_clear_pipelines.end(), [](auto pipeline) { return pipeline != nullptr; }); }
EMSCRIPTEN_KEEPALIVE void webgpu_shutdown() {
  closed = true;
  if (active) { active = false; completed(2, nullptr, 0, "GPU service shutdown"); }
  release_resources();
  if (surface) { wgpuSurfaceUnconfigure(surface); wgpuSurfaceRelease(surface); surface = nullptr; }
  if (sampler) { wgpuSamplerRelease(sampler); sampler = nullptr; }
  if (layout) { wgpuPipelineLayoutRelease(layout); layout = nullptr; }
  if (bindings) { wgpuBindGroupLayoutRelease(bindings); bindings = nullptr; }
  if (queue) { wgpuQueueRelease(queue); queue = nullptr; }
  if (device) { wgpuDeviceDestroy(device); wgpuDeviceRelease(device); device = nullptr; }
  if (adapter) { wgpuAdapterRelease(adapter); adapter = nullptr; }
  if (instance) { wgpuInstanceRelease(instance); instance = nullptr; }
}
EMSCRIPTEN_KEEPALIVE void webgpu_submit(uint32_t operation, uint32_t resource, const uint8_t* data, uint32_t size, uint32_t command) {
  if (closed || !device) { completed(2, nullptr, 0, "GPU service unavailable"); return; }
  if (active) { completed(1, nullptr, 0, "GPU service busy"); return; }
  active = true; reply.clear(); failure.clear(); created_resource = 0;
  wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
  try {
    if (size > MaxBytes) { throw std::runtime_error("GPU packet exceeds bounded capacity"); }
    std::span<const uint8_t> bytes(data, size);
    switch (static_cast<Opcode>(operation)) {
    case Opcode::Configure: {
      auto dimensions = packet<Dimensions>(bytes); exact(bytes, 0);
      if (!dimensions.width || !dimensions.height || dimensions.width > 8192 || dimensions.height > 8192) { throw std::runtime_error("invalid canvas dimensions"); }
      canvas_width = dimensions.width; canvas_height = dimensions.height;
      resize_canvas(dimensions.width, dimensions.height);
      configure_surface(surface_hdr);
      break;
    }
    case Opcode::CreateTexture: {
      if (!resource || textures.contains(resource) || pipelines.contains(resource) || resident_buffers.contains(resource) || textures.size() + pipelines.size() + resident_buffers.size() >= 4096) { throw std::runtime_error("duplicate or exhausted GPU resource ID"); }
      auto spec = packet<TexturePacket>(bytes);
      const uint32_t volumeDepth = spec.depth == 3 ? packet<uint32_t>(bytes) : 1;
      const uint32_t maximum = std::max({spec.width, spec.height, volumeDepth});
      const bool hdr = spec.depth == HdrTexture;
      if (hdr) { spec.depth = 0; }
      if (!spec.width || !spec.height || spec.width > 8192 || spec.height > 8192 || spec.depth > 3 || (hdr && (spec.levels != 1 || !bytes.empty())) ||
          (spec.depth == 2 && spec.width != spec.height) || !volumeDepth || volumeDepth > 2048 ||
          (spec.depth == 3 && (volumeDepth == 1 || spec.width > 2048 || spec.height > 2048)) ||
          !spec.levels || spec.levels > std::bit_width(maximum) || (spec.depth == 1 && spec.levels != 1)) {
        throw std::runtime_error("invalid texture dimensions or format");
      }
      const uint32_t faces = spec.depth == 2 ? 6 : 1;
      const uint64_t storageBytes = textureBytes(spec.width, spec.height, spec.levels, volumeDepth, hdr ? 8 : 4) * faces;
      // no pixels: a render target the backend fills itself (a scaled surface), left zero
      const bool empty = spec.depth == 0 && spec.levels == 1 && bytes.empty();
      exact(bytes, spec.depth == 1 || empty ? 0 : storageBytes);
      if (texture_bytes + storageBytes > TextureBudget) { throw budget_exhausted("resource", storageBytes); }
      auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
      descriptor.dimension = spec.depth == 3 ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
      descriptor.size = {spec.width, spec.height, spec.depth == 3 ? volumeDepth : faces}; descriptor.mipLevelCount = spec.levels;
      descriptor.format = spec.depth == 1 ? WGPUTextureFormat_Depth24PlusStencil8 : hdr ? hdr_format : WGPUTextureFormat_RGBA8Unorm;
      descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
      if (spec.depth != 3) { descriptor.usage |= WGPUTextureUsage_RenderAttachment; }
      if (spec.depth != 1) { descriptor.usage |= WGPUTextureUsage_CopyDst; }
      auto handle = wgpuDeviceCreateTexture(device, &descriptor);
      Texture stored{handle, spec.width, spec.height, spec.depth == 1, spec.levels, spec.depth == 2};
      stored.volumeDepth = volumeDepth; stored.hdr = hdr;
      textures.emplace(resource, stored); texture_bytes += storageBytes; created_resource = resource;
      if (spec.depth != 1 && !empty) {
        size_t offset = 0;
        for (uint32_t n = 0; n < spec.levels * faces; ++n) {
          const uint32_t level = n % spec.levels;
          WGPUExtent3D extent{std::max(spec.width >> level, 1u), std::max(spec.height >> level, 1u), std::max(volumeDepth >> level, 1u)};
          const size_t size = size_t(extent.width) * extent.height * extent.depthOrArrayLayers * 4;
          auto destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; destination.texture = handle; destination.mipLevel = level; destination.origin.z = n / spec.levels;
          auto layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT; layout.bytesPerRow = extent.width * 4; layout.rowsPerImage = extent.height;
          wgpuQueueWriteTexture(queue, &destination, bytes.data() + offset, size, &layout, &extent);
          offset += size;
        }
      }
      break;
    }
    case Opcode::CreatePipeline: {
      if (!resource || textures.contains(resource) || pipelines.contains(resource) || resident_buffers.contains(resource) || textures.size() + pipelines.size() + resident_buffers.size() >= 4096) { throw std::runtime_error("duplicate or exhausted GPU resource ID"); }
      if (pipelines.size() >= 256) { throw std::runtime_error("GPU pipeline budget exhausted"); }
      auto spec = packet<PipelinePacket>(bytes);
      const bool resident = spec.vertex_mask & ResidentVertexMask;
      const auto rawLayout = resident ? packet<RawLayout>(bytes) : RawLayout{};
      spec.vertex_mask &= ~ResidentVertexMask;
      exact(bytes, uint64_t(spec.vertex_bytes) + spec.fragment_bytes);
      if (spec.vertex_mask > 65535) { throw std::runtime_error("invalid vertex register mask"); }
      if ((spec.sample_count != 1 && spec.sample_count != 4) || spec.cube_mask > 15 || spec.shadow_mask > 15 || spec.volume_mask > 15 ||
          (spec.cube_mask & spec.shadow_mask) || (spec.volume_mask & (spec.cube_mask | spec.shadow_mask)) || spec.color_target > 1 || (!spec.color_target && spec.write_mask) || spec.color_format > 1) { throw std::runtime_error("invalid cube binding mask"); }
      if (spec.stencil > 1 || (spec.stencil && (!spec.depth || spec.stencil_compare < 1 || spec.stencil_compare > 8 || spec.stencil_fail < 1 || spec.stencil_fail > 8 || spec.stencil_depth_fail < 1 || spec.stencil_depth_fail > 8 || spec.stencil_pass < 1 || spec.stencil_pass > 8 || spec.stencil_read_mask > 255 || spec.stencil_write_mask > 255))) { throw std::runtime_error("invalid stencil pipeline state"); }
      auto shader = [](std::span<const uint8_t> code) {
        auto source = WGPU_SHADER_SOURCE_WGSL_INIT;
        source.code = {reinterpret_cast<const char*>(code.data()), code.size()};
        auto descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; descriptor.nextInChain = &source.chain;
        return wgpuDeviceCreateShaderModule(device, &descriptor);
      };
      std::array<WGPUVertexAttribute, 16> attributes{};
      uint32_t attributeCount = 0;
      for (uint32_t n = 0; n < attributes.size(); ++n) {
        if (spec.vertex_mask && !(spec.vertex_mask & (1u << n))) { continue; }
        auto& attribute = attributes[attributeCount];
        attribute = WGPU_VERTEX_ATTRIBUTE_INIT; attribute.format = WGPUVertexFormat_Float32x4;
        attribute.offset = attributeCount * 16; attribute.shaderLocation = n;
        ++attributeCount;
      }
      auto vertex_layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT; vertex_layout.arrayStride = attributeCount * 16; vertex_layout.attributeCount = attributeCount; vertex_layout.attributes = attributes.data();
      std::array<WGPUVertexBufferLayout, 16> rawLayouts{};
      std::array<WGPUVertexAttribute, 16> rawAttributes{};
      uint32_t rawStreams = 0, rawAttributesUsed = 0;
      if (resident) {
        for (uint32_t n = 0; n < 16; ++n) {
          auto& stream = rawLayouts[n]; stream = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
          stream.arrayStride = rawLayout.strides[n]; stream.attributes = rawAttributes.data() + rawAttributesUsed;
          const uint32_t streamNumber = n;
          for (uint32_t n = 0; n < 16; ++n) {
            if (!(spec.vertex_mask & (1u << n)) || rawLayout.attributes[n].stream != streamNumber) { continue; }
            const auto& source = rawLayout.attributes[n];
            if (!stream.arrayStride || stream.arrayStride > 2048 || stream.arrayStride % 4 || source.offset >= stream.arrayStride || !source.format || source.format > 41) { throw std::runtime_error("invalid resident vertex layout"); }
            auto& attribute = rawAttributes[rawAttributesUsed++]; attribute = WGPU_VERTEX_ATTRIBUTE_INIT;
            attribute.offset = source.offset; attribute.format = static_cast<WGPUVertexFormat>(source.format); attribute.shaderLocation = n;
            ++stream.attributeCount;
          }
          if (stream.attributeCount) { rawStreams = n + 1; }
        }
        if (!rawStreams || rawStreams > 8) { throw std::runtime_error("resident stream budget exceeded"); }
      }
      auto vertex = shader(bytes.first(spec.vertex_bytes)); auto fragment = shader(bytes.subspan(spec.vertex_bytes));
      auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = spec.color_format ? hdr_format : WGPUTextureFormat_RGBA8Unorm; target.writeMask = spec.write_mask;
      auto blend = WGPU_BLEND_STATE_INIT;
      blend.color.srcFactor = static_cast<WGPUBlendFactor>(spec.src_color); blend.color.dstFactor = static_cast<WGPUBlendFactor>(spec.dst_color); blend.color.operation = static_cast<WGPUBlendOperation>(spec.color_op);
      blend.alpha.srcFactor = static_cast<WGPUBlendFactor>(spec.src_alpha); blend.alpha.dstFactor = static_cast<WGPUBlendFactor>(spec.dst_alpha); blend.alpha.operation = static_cast<WGPUBlendOperation>(spec.alpha_op);
      if (spec.blend) { target.blend = &blend; }
      auto pixel = WGPU_FRAGMENT_STATE_INIT; pixel.module = fragment; pixel.entryPoint = text("fs_main"); pixel.targetCount = spec.color_target; pixel.targets = spec.color_target ? &target : nullptr;
      auto depth = WGPU_DEPTH_STENCIL_STATE_INIT; depth.format = WGPUTextureFormat_Depth24PlusStencil8; depth.depthWriteEnabled = spec.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False; depth.depthCompare = static_cast<WGPUCompareFunction>(spec.depth_compare);
      depth.stencilReadMask = depth.stencilWriteMask = 0;
      if (spec.stencil) {
        depth.stencilFront.compare = depth.stencilBack.compare = static_cast<WGPUCompareFunction>(spec.stencil_compare);
        depth.stencilFront.failOp = depth.stencilBack.failOp = static_cast<WGPUStencilOperation>(spec.stencil_fail);
        depth.stencilFront.depthFailOp = depth.stencilBack.depthFailOp = static_cast<WGPUStencilOperation>(spec.stencil_depth_fail);
        depth.stencilFront.passOp = depth.stencilBack.passOp = static_cast<WGPUStencilOperation>(spec.stencil_pass);
        depth.stencilReadMask = spec.stencil_read_mask; depth.stencilWriteMask = spec.stencil_write_mask;
      }
      auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
      const auto custom = (spec.cube_mask || spec.shadow_mask || spec.volume_mask) ? create_bindings(spec.cube_mask, spec.shadow_mask, spec.volume_mask) : std::pair<WGPUBindGroupLayout, WGPUPipelineLayout>{};
      descriptor.layout = (spec.cube_mask || spec.shadow_mask || spec.volume_mask) ? custom.second : layout; descriptor.vertex.module = vertex; descriptor.vertex.entryPoint = text("vs_main"); descriptor.vertex.bufferCount = resident ? rawStreams : 1; descriptor.vertex.buffers = resident ? rawLayouts.data() : &vertex_layout;
      descriptor.fragment = &pixel; descriptor.primitive.topology = static_cast<WGPUPrimitiveTopology>(spec.topology); descriptor.primitive.cullMode = static_cast<WGPUCullMode>(spec.cull); descriptor.primitive.frontFace = static_cast<WGPUFrontFace>(spec.front_face);
      if (spec.depth) { descriptor.depthStencil = &depth; }
      descriptor.multisample.count = spec.sample_count;
      auto pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor); pipelines.emplace(resource, Pipeline{pipeline, attributeCount * 16}); created_resource = resource;
      if (spec.cube_mask || spec.shadow_mask || spec.volume_mask) { wgpuPipelineLayoutRelease(custom.second); wgpuBindGroupLayoutRelease(custom.first); }
      wgpuShaderModuleRelease(vertex); wgpuShaderModuleRelease(fragment);
      break;
    }
    case Opcode::UploadBuffer: { upload_buffer(resource, bytes); break; }
    case Opcode::Draw: { draw(resource, bytes); break; }
    case Opcode::DrawBatch: { render_batch(resource, bytes, false); break; }
    case Opcode::RenderBatch: { render_batch(resource, bytes, true); break; }
    case Opcode::UploadDrawBatch: {
      const auto batch = packet<UploadDrawPacket>(bytes);
      exact(bytes, uint64_t(batch.upload_bytes) + batch.draw_bytes);
      if (batch.mixed > 1) { throw std::runtime_error("invalid upload draw batch kind"); }
      render_batch(resource, bytes.subspan(batch.upload_bytes), batch.mixed != 0, bytes.first(batch.upload_bytes), batch.upload_count);
      break;
    }
    case Opcode::Clear: { clear(resource, bytes); break; }
    case Opcode::UploadDepth: {
      const auto spec = packet<DepthUploadPacket>(bytes);
      auto& target = texture(resource);
      if (!target.depth || !spec.width || !spec.height || spec.width > target.width || spec.height > target.height ||
          (spec.depth_bits != 16 && spec.depth_bits != 24)) { throw std::runtime_error("invalid depth import target or encoding"); }
      exact(bytes, uint64_t(spec.width) * spec.height * 4);
      std::array<uint32_t, 8> mask{};
      for (size_t n = 0; n < bytes.size() / 4; ++n) {
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + n * 4, 4);
        if (spec.depth_bits == 16 && ((value >> 8) > 65535 || (value & 255))) { throw std::runtime_error("invalid D16 import value"); }
        mask[(value & 255) / 32] |= 1u << (value & 31);
      }
      if (!std::equal(mask.begin(), mask.end(), std::begin(spec.stencil_mask))) { throw std::runtime_error("depth import stencil mask does not cover its pixels"); }
      prepare_depth_upload();
      if (target.ms) { target.seed = true; target.stale = false; }
      auto values = buffer(bytes, WGPUBufferUsage_Storage);
      // a scaled depth target takes the console sized values nearest each of its pixels
      const std::array<uint32_t, 4> parameters{spec.width, spec.depth_bits == 24 ? 16777215u : 65535u,
        std::bit_cast<uint32_t>(float(spec.width) / float(target.width)), std::bit_cast<uint32_t>(float(spec.height) / float(target.height))};
      auto uniforms = buffer({reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)}, WGPUBufferUsage_Uniform);
      std::array<WGPUBindGroupEntry, 2> entries{};
      for (uint32_t n = 0; n < entries.size(); ++n) {
        entries[n] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[n].binding = n; entries[n].buffer = n ? uniforms : values;
        entries[n].size = n ? sizeof(parameters) : bytes.size();
      }
      auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
      group_descriptor.layout = depth_upload_bindings; group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
      auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
      auto depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
      depth.view = wgpuTextureCreateView(target.handle, nullptr);
      depth.depthLoadOp = depth.stencilLoadOp = WGPULoadOp_Clear;
      depth.depthStoreOp = depth.stencilStoreOp = WGPUStoreOp_Store;
      depth.depthClearValue = 0; depth.stencilClearValue = 0;
      auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.depthStencilAttachment = &depth;
      auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr); auto pass = begin_pass(encoder, &descriptor);
      wgpuRenderPassEncoderSetPipeline(pass, depth_upload_pipeline); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
      for (uint32_t n = 0; n < 256; ++n) {
        if (mask[n / 32] & (1u << (n & 31))) {
          wgpuRenderPassEncoderSetStencilReference(pass, n);
          wgpuRenderPassEncoderDraw(pass, 3, 1, 0, n);
        }
      }
      wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass); submit(encoder);
      wgpuTextureViewRelease(depth.view); wgpuBindGroupRelease(group);
      wgpuBufferRelease(values); wgpuBufferRelease(uniforms);
      break;
    }
    case Opcode::Copy: { copy(resource, bytes); break; }
    case Opcode::Blit: { blit(resource, bytes); break; }
    case Opcode::Multisample: { multisample(resource, bytes); break; }
    case Opcode::SoftShadows: { soft_shadows(resource, bytes); break; }
    case Opcode::ApplyShadows: { apply_shadows(resource, bytes); break; }
    case Opcode::Resolve: { resolve_hdr(resource, bytes); break; }
    case Opcode::Occlusion: { ambient_occlusion(resource, bytes); break; }
    case Opcode::SetGamma: {
      exact(bytes, gamma_ramp.size()); std::memcpy(gamma_ramp.data(), bytes.data(), gamma_ramp.size()); upload_gamma(); break;
    }
    case Opcode::Present: {
      auto present = packet<PresentPacket>(bytes); exact(bytes, 0);
      auto& source = texture(resource);
      if (source.depth || source.cube || source.volumeDepth > 1 || present.pillarbox > (PresentPillarbox | PresentLinear | PresentSmaa | PresentHdr | PresentColourBlind) ||
          !std::isfinite(present.headroom)) { throw std::runtime_error("unsupported presentation source or flags"); }
      prepare_presentation();
      if (const bool hdr = (present.pillarbox & PresentHdr) && !hdr_refused; hdr != surface_hdr) { configure_surface(hdr); }
      auto current = WGPU_SURFACE_TEXTURE_INIT; wgpuSurfaceGetCurrentTexture(surface, &current);
      if (!current.texture) { throw std::runtime_error("canvas surface unavailable"); }
      auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
      if (present.pillarbox & PresentSmaa) { run_smaa(encoder, source); }
      auto source_view = wgpuTextureCreateView(present.pillarbox & PresentSmaa ? smaa_state.targets[2] : source.handle, nullptr); auto ramp_view = wgpuTextureCreateView(gamma_texture, nullptr); auto target_view = wgpuTextureCreateView(presentation_target.handle, nullptr);
      const auto colour_matrix = colour_blind_rows((present.pillarbox & PresentColourBlind) >> PresentColourBlindShift);
      auto colour_uniforms = buffer({reinterpret_cast<const uint8_t*>(colour_matrix.data()), sizeof(colour_matrix)}, WGPUBufferUsage_Uniform);
      std::array<WGPUBindGroupEntry, 3> entries{};
      entries[0] = WGPU_BIND_GROUP_ENTRY_INIT; entries[0].binding = 0; entries[0].textureView = source_view;
      entries[1] = WGPU_BIND_GROUP_ENTRY_INIT; entries[1].binding = 1; entries[1].textureView = ramp_view;
      entries[2] = WGPU_BIND_GROUP_ENTRY_INIT; entries[2].binding = 2; entries[2].buffer = colour_uniforms; entries[2].size = sizeof(colour_matrix);
      auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = presentation_bindings; group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data(); auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
      auto color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; color.view = target_view; color.loadOp = WGPULoadOp_Clear; color.storeOp = WGPUStoreOp_Store; color.clearValue = {0, 0, 0, 1};
      auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &color;
      auto pass = begin_pass(encoder, &descriptor);
      float width = canvas_width, height = canvas_height;
      if (present.pillarbox & PresentPillarbox) { if (width * 3 > height * 4) { width = height * 4 / 3; } else { height = width * 3 / 4; } }
      wgpuRenderPassEncoderSetViewport(pass, (canvas_width - width) / 2, (canvas_height - height) / 2, width, height, 0, 1);
      wgpuRenderPassEncoderSetPipeline(pass, present.pillarbox & PresentLinear ? presentation_linear_pipeline : presentation_pipeline); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr); wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
      if (surface_hdr) { expand_frame(encoder, current.texture, present.headroom); }
      else { auto from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; from.texture = presentation_target.handle; auto to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; to.texture = current.texture; WGPUExtent3D extent{canvas_width, canvas_height, 1}; wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent); }
      submit(encoder);
      wgpuBindGroupRelease(group); wgpuBufferRelease(colour_uniforms); wgpuTextureViewRelease(source_view); wgpuTextureViewRelease(ramp_view); wgpuTextureViewRelease(target_view); wgpuTextureRelease(current.texture);
      auto info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT; info.mode = WGPUCallbackMode_AllowSpontaneous;
      info.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView message, void*, void*) {
        if (status != WGPUQueueWorkDoneStatus_Success) { failure = string(message); }
        --presents_in_flight;
        if (present_waiting && presents_in_flight <= 1) { present_waiting = false; if (active && device) { finish(); } }
      };
      wgpuQueueOnSubmittedWorkDone(queue, info);
      frame_stats[StatTextureMiB] = uint32_t(texture_bytes >> 20);
      frame_stats[StatUploadKiB] = uint32_t(std::exchange(frame_upload_bytes, 0) >> 10);
      presented_stats = frame_stats; frame_stats = {};
      if (++presents_in_flight > 1) { present_waiting = true; return; }
      break;
    }
    case Opcode::ReadbackCollect: {
      const bool poll = bytes.size() == 4 && packet<uint32_t>(bytes) == 1;
      exact(bytes, 0);
      const auto found = async_reads.find(resource);
      if (found == async_reads.end()) { throw std::runtime_error("no started asynchronous readback"); }
      if (!found->second.done && poll) { break; }
      if (!found->second.done) { collect_waiting = resource; return; }
      reply = std::move(found->second.bytes); failure = std::move(found->second.failure); async_reads.erase(found);
      break;
    }
    case Opcode::ReadbackStart:
    case Opcode::Readback: {
      const bool start = static_cast<Opcode>(operation) == Opcode::ReadbackStart;
      const uint32_t slot = start && bytes.size() == 4 ? packet<uint32_t>(bytes) + 1 : 0;
      if (start && (!resource || slot > ReadbackSlots || (!slot && (async_reads.size() >= 64 || async_reads.contains(resource))))) { throw std::runtime_error("invalid asynchronous readback"); }
      if (resource) { readable(texture(resource)); }
      auto target = resource ? texture(resource) : presentation_target;
      bool stencil = false;
#ifdef WEBGPU_SERVICE_TESTING
      if (target.depth && bytes.size() == 4) {
        if (packet<uint32_t>(bytes) != 1) { throw std::runtime_error("unsupported readback plane"); }
        stencil = true;
      }
#endif
      exact(bytes, 0);
      if (!target.handle) { throw std::runtime_error("no completed presentation target"); }
      if (target.cube || target.volumeDepth > 1) { throw std::runtime_error("readback requires a 2D texture"); }
      if (target.hdr) { throw std::runtime_error("an HDR scene target reads back through a resolved RGBA8 copy"); }
      const uint32_t pixel_bytes = stencil ? 1 : 4;
      const uint32_t pitch = (target.width * pixel_bytes + 255) & ~255u;
      if (uint64_t(pitch) * target.height > MaxBytes) { throw std::runtime_error("readback exceeds bounded capacity"); }
      const uint32_t stencilPitch = target.depth && !stencil ? (target.width + 255) & ~255u : 0;
      const uint32_t mapBytes = (pitch + stencilPitch) * target.height;
      if (stencilPitch && texture_bytes > TextureBudget - uint64_t(target.width) * target.height * 4) {
        throw std::runtime_error("GPU depth readback texture budget exhausted");
      }
      auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT; descriptor.size = mapBytes; descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
      auto staging = wgpuDeviceCreateBuffer(device, &descriptor);
      auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
      auto packed = stencilPitch ? pack_depth(encoder, target) : nullptr;
      auto source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; source.texture = packed ? packed : target.handle;
      if (stencil) { source.aspect = WGPUTextureAspect_StencilOnly; }
      auto destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT; destination.buffer = staging; destination.layout.bytesPerRow = pitch; destination.layout.rowsPerImage = target.height;
      WGPUExtent3D extent{target.width, target.height, 1}; wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
      if (stencilPitch) {
        source.texture = target.handle; source.aspect = WGPUTextureAspect_StencilOnly;
        destination.layout.offset = uint64_t(pitch) * target.height; destination.layout.bytesPerRow = stencilPitch;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
      }
      submit(encoder);
      struct Read { WGPUBuffer buffer; WGPUTexture packed; uint32_t width, height, pitch, pixel_bytes, stencil_pitch, map_bytes, async, slot, resource; };
      auto read = new Read{staging, packed, target.width, target.height, pitch, pixel_bytes, stencilPitch, mapBytes, start && !slot ? resource : 0, slot, resource};
      if (start && !slot) { async_reads[resource] = {}; }
      if (slot) { slot_reads[slot - 1] = {}; }
      auto info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT; info.mode = WGPUCallbackMode_AllowSpontaneous; info.userdata1 = read;
      info.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* userdata, void*) {
        auto read = static_cast<Read*>(userdata);
        const auto found = read->async ? async_reads.find(read->async) : async_reads.end();
        auto& output = read->slot ? slot_reads[read->slot - 1].bytes : found != async_reads.end() ? found->second.bytes : reply;
        auto& error = read->slot ? slot_reads[read->slot - 1].failure : found != async_reads.end() ? found->second.failure : failure;
        if (status == WGPUMapAsyncStatus_Success) {
          auto mapped = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(read->buffer, 0, read->map_bytes));
          output.resize(read->width * read->height * read->pixel_bytes);
          for (uint32_t n = 0; n < read->height; ++n) {
            auto row = output.data() + n * read->width * read->pixel_bytes;
            std::memcpy(row, mapped + n * read->pitch, read->width * read->pixel_bytes);
            if (read->stencil_pitch) {
              const auto stencil = mapped + read->pitch * read->height + n * read->stencil_pitch;
              for (uint32_t n = 0; n < read->width; ++n) { row[n * 4] = stencil[n]; }
            }
          }
          wgpuBufferUnmap(read->buffer);
        }
        else { error = string(message); }
        wgpuBufferDestroy(read->buffer); wgpuBufferRelease(read->buffer);
        if (read->packed) { wgpuTextureDestroy(read->packed); wgpuTextureRelease(read->packed); }
        const uint32_t async = read->async, slot = read->slot, resource = read->resource;
        delete read;
        if (slot) {
          auto& result = slot_reads[slot - 1];
          deliver_readback(slot - 1, resource, result.failure.empty() ? 0 : 1, result.bytes.data(), uint32_t(result.bytes.size()));
          result = {};
          return;
        }
        if (async && found == async_reads.end()) { return; }
        if (found != async_reads.end()) {
          found->second.done = true;
          if (collect_waiting != async) { return; }
          collect_waiting = 0;
          reply = std::move(found->second.bytes); failure = std::move(found->second.failure); async_reads.erase(found);
        }
        if (active && device) { finish(); }
      };
      wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, descriptor.size, info);
      if (start) { break; }
      return;
    }
    case Opcode::Destroy: {
      exact(bytes, 0);
      if (const auto found = resident_buffers.find(resource); found != resident_buffers.end()) {
        resident_buffer_bytes -= found->second.bytes; wgpuBufferDestroy(found->second.handle); wgpuBufferRelease(found->second.handle); resident_buffers.erase(found); break;
      }
      if (!textures.contains(resource) && !pipelines.contains(resource)) { throw std::runtime_error("unknown GPU resource"); }
      discard(resource);
      break;
    }
    case Opcode::Flush: {
      exact(bytes, 0); auto info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT; info.mode = WGPUCallbackMode_AllowSpontaneous;
      info.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView message, void*, void*) { if (status != WGPUQueueWorkDoneStatus_Success) { failure = string(message); } if (active && device) { finish(); } };
      wgpuQueueOnSubmittedWorkDone(queue, info); return;
    }
    case Opcode::Shutdown: release_resources(); break;
    default: throw std::runtime_error("unsupported GPU opcode");
    }
  } catch (const std::exception& error) { failure = error.what(); }
  const auto opcode = static_cast<Opcode>(operation);
  if (failure.empty() && pending_validations < 64 && (opcode == Opcode::Draw || opcode == Opcode::DrawBatch || opcode == Opcode::RenderBatch ||
      opcode == Opcode::Clear || opcode == Opcode::Copy || opcode == Opcode::Destroy || opcode == Opcode::UploadBuffer || opcode == Opcode::UploadDrawBatch || opcode == Opcode::ReadbackStart)) {
    created_resource = 0;
    defer_validation(command, resource);
    return;
  }
  finish();
}
}
