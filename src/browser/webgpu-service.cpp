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
};
struct CachedSampler { WGPUSampler handle; uint64_t used; };
std::map<std::array<uint32_t, 8>, CachedSampler> sampler_cache;
uint64_t sampler_sequence = 0;
uint32_t cached_views = 0;
WGPUSampler cached_sampler(const uint32_t (&state)[7], uint32_t addressW) {
  std::array<uint32_t, 8> key;
  std::copy(std::begin(state), std::end(state), key.begin());
  key[7] = addressW;
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
}
uint64_t textureBytes(uint32_t width, uint32_t height, uint32_t levels, uint32_t depth = 1) {
  uint64_t bytes = 0;
  for (uint32_t n = 0; n < levels; ++n) { bytes += uint64_t(std::max(width >> n, 1u)) * std::max(height >> n, 1u) * std::max(depth >> n, 1u) * 4; }
  return bytes;
}
std::map<uint32_t, Texture> textures;
struct Pipeline { WGPURenderPipeline handle; uint32_t vertexStride; };
std::map<uint32_t, Pipeline> pipelines;
std::vector<uint8_t> reply;
std::string failure;
bool closed = false;
bool active = false;
uint32_t pending_validations = 0;
bool validation_barrier = false;
uint32_t created_resource = 0;
uint64_t texture_bytes = 0;
constexpr uint64_t TextureBudget = 256ull * 1024 * 1024;
uint32_t canvas_width = 0, canvas_height = 0;
WGPUTexture gamma_texture;
WGPURenderPipeline presentation_pipeline;
WGPUBindGroupLayout presentation_bindings;
WGPUPipelineLayout presentation_layout;
Texture presentation_target{};
WGPURenderPipeline depth_upload_pipeline;
WGPUBindGroupLayout depth_upload_bindings;
WGPUPipelineLayout depth_upload_layout;
WGPURenderPipeline depth_readback_pipeline;
WGPUBindGroupLayout depth_readback_bindings;
WGPUPipelineLayout depth_readback_layout;
std::array<WGPURenderPipeline, 9> partial_clear_pipelines{};
WGPUBindGroupLayout partial_clear_bindings;
WGPUPipelineLayout partial_clear_layout;
std::array<uint8_t, 768> gamma_ramp = [] {
  std::array<uint8_t, 768> result{};
  for (uint32_t n = 0; n < result.size(); ++n) { result[n] = n % 256; }
  return result;
}();
void discard(uint32_t id) {
  auto image = textures.find(id);
  if (image != textures.end()) { texture_bytes -= textureBytes(image->second.width, image->second.height, image->second.levels, image->second.volumeDepth) * (image->second.cube ? 6 : 1); release_views(image->second); wgpuTextureDestroy(image->second.handle); wgpuTextureRelease(image->second.handle); textures.erase(image); }
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
void submit(WGPUCommandEncoder encoder) {
  auto command = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(queue, 1, &command);
  wgpuCommandBufferRelease(command);
  wgpuCommandEncoderRelease(encoder);
}
WGPUBuffer buffer(std::span<const uint8_t> bytes, WGPUBufferUsage usage) {
  auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
  descriptor.size = (bytes.size() + 3) & ~uint64_t(3);
  descriptor.usage = usage | WGPUBufferUsage_CopyDst;
  auto result = wgpuDeviceCreateBuffer(device, &descriptor);
  std::vector<uint8_t> padded(descriptor.size);
  std::memcpy(padded.data(), bytes.data(), bytes.size());
  wgpuQueueWriteBuffer(queue, result, 0, padded.data(), padded.size());
  return result;
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
  const uint32_t flags = key == 8 ? 1 : key;
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
  auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = WGPUTextureFormat_RGBA8Unorm;
  target.writeMask = key == 8 ? WGPUColorWriteMask_Alpha : WGPUColorWriteMask_All;
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
  if (flags & 6) { descriptor.depthStencil = &depth; }
  partial_clear_pipelines[key] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
  wgpuShaderModuleRelease(shader);
  return partial_clear_pipelines[key];
}
void force_opaque_alpha(WGPUCommandEncoder encoder, Texture& target, const CopyPacket& copy) {
  const auto pipeline = partial_clear_pipeline(8);
  const std::array<float, 8> parameters{0, 0, 0, 1};
  auto uniforms = buffer({reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)}, WGPUBufferUsage_Uniform);
  auto entry = WGPU_BIND_GROUP_ENTRY_INIT; entry.binding = 0; entry.buffer = uniforms; entry.size = sizeof(parameters);
  auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = partial_clear_bindings; group_descriptor.entryCount = 1; group_descriptor.entries = &entry;
  auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
  auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  attachment.view = wgpuTextureCreateView(target.handle, nullptr); attachment.loadOp = WGPULoadOp_Load; attachment.storeOp = WGPUStoreOp_Store;
  auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &attachment;
  auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
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
  auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
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
struct Parameters { width: u32, maximum: u32, padding: vec2<u32> }
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
  let value = pixels[u32(input.position.y) * parameters.width + u32(input.position.x)];
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
@vertex fn vs_main(@builtin(vertex_index) vertex: u32) -> Output {
  let positions = array<vec2<f32>, 3>(vec2<f32>(-1,-1), vec2<f32>(3,-1), vec2<f32>(-1,3));
  let position = positions[vertex];
  var output: Output; output.position = vec4<f32>(position,0,1); output.uv = vec2<f32>((position.x+1)*0.5,(1-position.y)*0.5); return output;
}
@fragment fn fs_main(input: Output) -> @location(0) vec4<f32> {
  let dimensions = textureDimensions(source);
  let at = clamp(vec2<i32>(floor(input.uv * vec2<f32>(dimensions))),vec2<i32>(0),vec2<i32>(dimensions)-vec2<i32>(1));
  let color = textureLoad(source,at,0);
  let r = textureLoad(ramp,vec2<i32>(i32(round(clamp(color.r,0,1)*255)),0),0).r;
  let g = textureLoad(ramp,vec2<i32>(i32(round(clamp(color.g,0,1)*255)),0),0).g;
  let b = textureLoad(ramp,vec2<i32>(i32(round(clamp(color.b,0,1)*255)),0),0).b;
  return vec4<f32>(r,g,b,color.a);
})";
    auto source = WGPU_SHADER_SOURCE_WGSL_INIT; source.code = text(code);
    auto shader_descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader_descriptor.nextInChain = &source.chain;
    auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    for (uint32_t n = 0; n < entries.size(); ++n) { entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT; entries[n].binding = n; entries[n].visibility = WGPUShaderStage_Fragment; entries[n].texture.sampleType = WGPUTextureSampleType_Float; entries[n].texture.viewDimension = WGPUTextureViewDimension_2D; }
    auto bindings = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bindings.entryCount = entries.size(); bindings.entries = entries.data(); presentation_bindings = wgpuDeviceCreateBindGroupLayout(device, &bindings);
    auto layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; layout.bindGroupLayoutCount = 1; layout.bindGroupLayouts = &presentation_bindings; presentation_layout = wgpuDeviceCreatePipelineLayout(device, &layout);
    auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = WGPUTextureFormat_RGBA8Unorm;
    auto fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = shader; fragment.entryPoint = text("fs_main"); fragment.targetCount = 1; fragment.targets = &target;
    auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT; descriptor.layout = presentation_layout; descriptor.vertex.module = shader; descriptor.vertex.entryPoint = text("vs_main"); descriptor.fragment = &fragment;
    presentation_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor); wgpuShaderModuleRelease(shader);
  }
  if (presentation_target.width != canvas_width || presentation_target.height != canvas_height) {
    const uint64_t old_size = uint64_t(presentation_target.width) * presentation_target.height * 4;
    if (texture_bytes - old_size + uint64_t(canvas_width) * canvas_height * 4 > TextureBudget) { throw std::runtime_error("GPU presentation texture budget exhausted"); }
    release_presentation_target();
    auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT; descriptor.size = {canvas_width, canvas_height, 1}; descriptor.format = WGPUTextureFormat_RGBA8Unorm; descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    presentation_target = {wgpuDeviceCreateTexture(device, &descriptor), canvas_width, canvas_height, false}; texture_bytes += uint64_t(canvas_width) * canvas_height * 4;
  }
}
void draw(uint32_t resource, std::span<const uint8_t> bytes, const DrawUploads* uploads = nullptr, uint32_t upload = 0) {
      auto spec = packet<DrawPacket>(bytes); exact(bytes, uint64_t(spec.vertex_bytes) + spec.index_bytes + spec.uniform_bytes);
      for (const float value : spec.blend_constant) {
        if (!std::isfinite(value) || value < 0 || value > 1) { throw std::runtime_error("invalid blend constant"); }
      }
      auto found = pipelines.find(spec.pipeline); if (found == pipelines.end()) { throw std::runtime_error("unknown pipeline resource"); }
      const uint32_t stride = found->second.vertexStride;
      auto* target = resource ? &texture(resource) : nullptr;
      if ((!target && !spec.depth_target) || (target && (target->depth || target->cube || target->volumeDepth > 1)) || !spec.vertex_bytes || spec.vertex_bytes % stride || spec.vertex_count > spec.vertex_bytes / stride || spec.index32 > 1 || spec.index_bytes != uint64_t(spec.index_count) * (spec.index32 ? 4 : 2) || !spec.uniform_bytes || spec.uniform_bytes > 65536 || spec.uniform_bytes % 16) { throw std::runtime_error("invalid draw payload"); }
      for (uint32_t n = 0; n < spec.index_count; ++n) {
        uint32_t index = 0;
        std::memcpy(&index, bytes.data() + spec.vertex_bytes + n * (spec.index32 ? 4 : 2), spec.index32 ? 4 : 2);
        if (index >= spec.vertex_bytes / stride) { throw std::runtime_error("draw index exceeds owned vertices"); }
      }
      for (uint32_t n = 0; n < 4; ++n) {
        if (spec.texture[n] == spec.depth_target || spec.texture[n] == resource) { throw std::runtime_error("invalid sampled texture resource"); }
        if (spec.sampler[n][0] < 1 || spec.sampler[n][0] > 3 || spec.sampler[n][1] < 1 || spec.sampler[n][1] > 3 || spec.sampler[n][2] < 1 || spec.sampler[n][2] > 2 || spec.sampler[n][3] < 1 || spec.sampler[n][3] > 2 || spec.sampler[n][4] < 1 || spec.sampler[n][4] > 2 || spec.sampler[n][5] > spec.sampler[n][6] || spec.sampler[n][6] > 32 || spec.address_w[n] < 1 || spec.address_w[n] > 3) { throw std::runtime_error("unsupported sampler state"); }
      }
      if (spec.depth_target && !texture(spec.depth_target).depth) { throw std::runtime_error("invalid depth target resource"); }
      const std::array<uint64_t, 3> offsets = uploads ? uploads->offsets[upload] : std::array<uint64_t, 3>{};
      auto vertices = uploads ? uploads->buffers[0] : buffer(bytes.first(spec.vertex_bytes), WGPUBufferUsage_Vertex);
      auto uniforms = uploads ? uploads->buffers[2] : buffer(bytes.subspan(spec.vertex_bytes + spec.index_bytes), WGPUBufferUsage_Uniform);
      WGPUBuffer indices = nullptr;
      if (spec.index_count) { indices = uploads ? uploads->buffers[1] : buffer(bytes.subspan(spec.vertex_bytes, spec.index_bytes), WGPUBufferUsage_Index); }
      std::array<WGPUBindGroupEntry, 9> entries{}; std::array<WGPUTextureView, 4> views{}; std::array<WGPUSampler, 4> samplers{};
      entries[0] = WGPU_BIND_GROUP_ENTRY_INIT; entries[0].binding = 0; entries[0].buffer = uniforms; entries[0].offset = offsets[2]; entries[0].size = spec.uniform_bytes;
      for (uint32_t n = 0; n < 4; ++n) {
        auto& sampled = texture(spec.texture[n]);
        samplers[n] = cached_sampler(spec.sampler[n], spec.address_w[n]);
        views[n] = sampled_view(sampled);
        entries[1 + n * 2] = WGPU_BIND_GROUP_ENTRY_INIT; entries[1 + n * 2].binding = 1 + n * 2; entries[1 + n * 2].textureView = views[n];
        entries[2 + n * 2] = WGPU_BIND_GROUP_ENTRY_INIT; entries[2 + n * 2].binding = 2 + n * 2; entries[2 + n * 2].sampler = samplers[n];
      }
      auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = wgpuRenderPipelineGetBindGroupLayout(found->second.handle, 0); group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
      auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
      wgpuBindGroupLayoutRelease(group_descriptor.layout);
      auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
      auto pass_descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
      if (target) {
        attachment.view = attachment_view(*target); attachment.loadOp = WGPULoadOp_Load; attachment.storeOp = WGPUStoreOp_Store;
        pass_descriptor.colorAttachmentCount = 1; pass_descriptor.colorAttachments = &attachment;
      }
      auto depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
      if (spec.depth_target) { depth.depthClearValue = 1; depth.view = attachment_view(texture(spec.depth_target)); depth.depthLoadOp = WGPULoadOp_Load; depth.depthStoreOp = WGPUStoreOp_Store; depth.stencilLoadOp = WGPULoadOp_Load; depth.stencilStoreOp = WGPUStoreOp_Store; pass_descriptor.depthStencilAttachment = &depth; }
      auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
      if (batch_encoder && batch_pass && (batch_color != resource || batch_depth != spec.depth_target)) { end_batch_pass(); }
      auto pass = batch_encoder && batch_pass ? batch_pass : wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
      if (batch_encoder) { batch_pass = pass; batch_color = resource; batch_depth = spec.depth_target; }
      wgpuRenderPassEncoderSetPipeline(pass, found->second.handle); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr); wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertices, offsets[0], spec.vertex_bytes);
      wgpuRenderPassEncoderSetStencilReference(pass, spec.stencil_reference);
      const WGPUColor blendConstant{spec.blend_constant[0], spec.blend_constant[1], spec.blend_constant[2], spec.blend_constant[3]};
      wgpuRenderPassEncoderSetBlendConstant(pass, &blendConstant);
      wgpuRenderPassEncoderSetViewport(pass, spec.viewport[0], spec.viewport[1], spec.viewport[2], spec.viewport[3], spec.viewport[4], spec.viewport[5]); wgpuRenderPassEncoderSetScissorRect(pass, spec.scissor[0], spec.scissor[1], spec.scissor[2], spec.scissor[3]);
      if (indices) { wgpuRenderPassEncoderSetIndexBuffer(pass, indices, spec.index32 ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16, offsets[1], spec.index_bytes); wgpuRenderPassEncoderDrawIndexed(pass, spec.index_count, 1, 0, 0, 0); }
      else { wgpuRenderPassEncoderDraw(pass, spec.vertex_count, 1, 0, 0); }
      if (!batch_encoder) { wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass); submit(encoder); }
      for (auto item : samplers) { wgpuSamplerRelease(item); }
      wgpuBindGroupRelease(group);
      if (!uploads) { wgpuBufferRelease(vertices); wgpuBufferRelease(uniforms); if (indices) { wgpuBufferRelease(indices); } }
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
      if (target) {
        color.view = wgpuTextureCreateView(target->handle, nullptr); color.loadOp = spec.rect_count ? WGPULoadOp_Load : WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store; color.clearValue = {spec.color[0], spec.color[1], spec.color[2], spec.color[3]};
        descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &color;
      }
      if (depth_target) {
        depth.view = wgpuTextureCreateView(depth_target->handle, nullptr);
        depth.depthLoadOp = !spec.rect_count && (spec.flags & 2) ? WGPULoadOp_Clear : WGPULoadOp_Load;
        depth.stencilLoadOp = !spec.rect_count && (spec.flags & 4) ? WGPULoadOp_Clear : WGPULoadOp_Load;
        depth.depthStoreOp = WGPUStoreOp_Store; depth.stencilStoreOp = WGPUStoreOp_Store;
        depth.depthClearValue = spec.depth; depth.stencilClearValue = spec.stencil; descriptor.depthStencilAttachment = &depth;
      }
      WGPUBuffer uniforms = nullptr; WGPUBindGroup group = nullptr;
      WGPURenderPipeline pipeline = nullptr;
      if (spec.rect_count) {
        pipeline = partial_clear_pipeline(spec.flags);
        std::array<float, 8> parameters{spec.color[0], spec.color[1], spec.color[2], spec.color[3], spec.depth};
        uniforms = buffer({reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)}, WGPUBufferUsage_Uniform);
        auto entry = WGPU_BIND_GROUP_ENTRY_INIT; entry.binding = 0; entry.buffer = uniforms; entry.size = sizeof(parameters);
        auto descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; descriptor.layout = partial_clear_bindings; descriptor.entryCount = 1; descriptor.entries = &entry;
        group = wgpuDeviceCreateBindGroup(device, &descriptor);
      }
      end_batch_pass();
      auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr);
      auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
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
void copy(uint32_t resource, std::span<const uint8_t> bytes) {
      auto copy = packet<CopyPacket>(bytes); exact(bytes, 0);
      auto& source = texture(copy.source); auto& target = texture(resource);
      if (source.depth || target.depth || source.cube || target.cube || source.volumeDepth > 1 || target.volumeDepth > 1 || copy.force_opaque > 1 || !copy.width || !copy.height || uint64_t(copy.source_x) + copy.width > source.width || uint64_t(copy.source_y) + copy.height > source.height || uint64_t(copy.destination_x) + copy.width > target.width || uint64_t(copy.destination_y) + copy.height > target.height || copy.source == resource) { throw std::runtime_error("invalid texture copy region, format or alias"); }
      auto from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; from.texture = source.handle; from.origin = {copy.source_x, copy.source_y, 0}; auto to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; to.texture = target.handle; to.origin = {copy.destination_x, copy.destination_y, 0};
      WGPUExtent3D extent{copy.width, copy.height, 1};
      end_batch_pass();
      auto encoder = batch_encoder ? batch_encoder : wgpuDeviceCreateCommandEncoder(device, nullptr); wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
      if (copy.force_opaque) { force_opaque_alpha(encoder, target, copy); }
      if (!batch_encoder) { submit(encoder); }
}
void release_resources() {
  for (const auto& [key, item] : sampler_cache) { wgpuSamplerRelease(item.handle); }
  sampler_cache.clear(); sampler_sequence = 0;
  for (auto& pipeline : partial_clear_pipelines) { if (pipeline) { wgpuRenderPipelineRelease(pipeline); pipeline = nullptr; } }
  if (partial_clear_layout) { wgpuPipelineLayoutRelease(partial_clear_layout); partial_clear_layout = nullptr; }
  if (partial_clear_bindings) { wgpuBindGroupLayoutRelease(partial_clear_bindings); partial_clear_bindings = nullptr; }
  if (depth_upload_pipeline) { wgpuRenderPipelineRelease(depth_upload_pipeline); depth_upload_pipeline = nullptr; }
  if (depth_upload_layout) { wgpuPipelineLayoutRelease(depth_upload_layout); depth_upload_layout = nullptr; }
  if (depth_upload_bindings) { wgpuBindGroupLayoutRelease(depth_upload_bindings); depth_upload_bindings = nullptr; }
  if (depth_readback_pipeline) { wgpuRenderPipelineRelease(depth_readback_pipeline); depth_readback_pipeline = nullptr; }
  if (depth_readback_layout) { wgpuPipelineLayoutRelease(depth_readback_layout); depth_readback_layout = nullptr; }
  if (depth_readback_bindings) { wgpuBindGroupLayoutRelease(depth_readback_bindings); depth_readback_bindings = nullptr; }
  release_presentation_target();
  if (gamma_texture) { wgpuTextureDestroy(gamma_texture); wgpuTextureRelease(gamma_texture); gamma_texture = nullptr; }
  if (presentation_pipeline) { wgpuRenderPipelineRelease(presentation_pipeline); presentation_pipeline = nullptr; }
  if (presentation_layout) { wgpuPipelineLayoutRelease(presentation_layout); presentation_layout = nullptr; }
  if (presentation_bindings) { wgpuBindGroupLayoutRelease(presentation_bindings); presentation_bindings = nullptr; }
  for (auto& entry : textures) { release_views(entry.second); wgpuTextureDestroy(entry.second.handle); wgpuTextureRelease(entry.second.handle); }
  textures.clear(); texture_bytes = 0;
  for (auto& entry : pipelines) { wgpuRenderPipelineRelease(entry.second.handle); }
  pipelines.clear();
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
EMSCRIPTEN_KEEPALIVE uint32_t webgpu_resources() { return textures.size() + pipelines.size() + sampler_cache.size() + cached_views + (gamma_texture ? 1 : 0) + (presentation_pipeline ? 1 : 0) + (presentation_target.handle ? 1 : 0) + (depth_upload_pipeline ? 1 : 0) + (depth_readback_pipeline ? 1 : 0) + std::count_if(partial_clear_pipelines.begin(), partial_clear_pipelines.end(), [](auto pipeline) { return pipeline != nullptr; }); }
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
      auto configuration = WGPU_SURFACE_CONFIGURATION_INIT;
      configuration.device = device; configuration.format = WGPUTextureFormat_RGBA8Unorm;
      configuration.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
      configuration.width = dimensions.width; configuration.height = dimensions.height;
      configuration.presentMode = WGPUPresentMode_Fifo; configuration.alphaMode = WGPUCompositeAlphaMode_Opaque;
      wgpuSurfaceConfigure(surface, &configuration);
      break;
    }
    case Opcode::CreateTexture: {
      if (!resource || textures.contains(resource) || pipelines.contains(resource) || textures.size() + pipelines.size() >= 4096) { throw std::runtime_error("duplicate or exhausted GPU resource ID"); }
      auto spec = packet<TexturePacket>(bytes);
      const uint32_t volumeDepth = spec.depth == 3 ? packet<uint32_t>(bytes) : 1;
      const uint32_t maximum = std::max({spec.width, spec.height, volumeDepth});
      if (!spec.width || !spec.height || spec.width > 8192 || spec.height > 8192 || spec.depth > 3 ||
          (spec.depth == 2 && spec.width != spec.height) || !volumeDepth || volumeDepth > 2048 ||
          (spec.depth == 3 && (volumeDepth == 1 || spec.width > 2048 || spec.height > 2048)) ||
          !spec.levels || spec.levels > std::bit_width(maximum) || (spec.depth == 1 && spec.levels != 1)) {
        throw std::runtime_error("invalid texture dimensions or format");
      }
      const uint32_t faces = spec.depth == 2 ? 6 : 1;
      const uint64_t storageBytes = textureBytes(spec.width, spec.height, spec.levels, volumeDepth) * faces;
      exact(bytes, spec.depth == 1 ? 0 : storageBytes);
      if (texture_bytes + storageBytes > TextureBudget) { throw std::runtime_error("GPU texture budget exhausted"); }
      auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
      descriptor.dimension = spec.depth == 3 ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
      descriptor.size = {spec.width, spec.height, spec.depth == 3 ? volumeDepth : faces}; descriptor.mipLevelCount = spec.levels;
      descriptor.format = spec.depth == 1 ? WGPUTextureFormat_Depth24PlusStencil8 : WGPUTextureFormat_RGBA8Unorm;
      descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
      if (spec.depth != 3) { descriptor.usage |= WGPUTextureUsage_RenderAttachment; }
      if (spec.depth != 1) { descriptor.usage |= WGPUTextureUsage_CopyDst; }
      auto handle = wgpuDeviceCreateTexture(device, &descriptor);
      Texture stored{handle, spec.width, spec.height, spec.depth == 1, spec.levels, spec.depth == 2};
      stored.volumeDepth = volumeDepth;
      textures.emplace(resource, stored); texture_bytes += storageBytes; created_resource = resource;
      if (spec.depth != 1) {
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
      if (!resource || textures.contains(resource) || pipelines.contains(resource) || textures.size() + pipelines.size() >= 4096) { throw std::runtime_error("duplicate or exhausted GPU resource ID"); }
      if (pipelines.size() >= 256) { throw std::runtime_error("GPU pipeline budget exhausted"); }
      auto spec = packet<PipelinePacket>(bytes); exact(bytes, uint64_t(spec.vertex_bytes) + spec.fragment_bytes);
      if (spec.vertex_mask > 65535) { throw std::runtime_error("invalid vertex register mask"); }
      if (spec.cube_mask > 15 || spec.shadow_mask > 15 || spec.volume_mask > 15 ||
          (spec.cube_mask & spec.shadow_mask) || (spec.volume_mask & (spec.cube_mask | spec.shadow_mask)) || spec.color_target > 1 || (!spec.color_target && spec.write_mask)) { throw std::runtime_error("invalid cube binding mask"); }
      if (spec.stencil > 1 || (spec.stencil && (!spec.depth || spec.stencil_compare < 1 || spec.stencil_compare > 8 || spec.stencil_fail < 1 || spec.stencil_fail > 8 || spec.stencil_depth_fail < 1 || spec.stencil_depth_fail > 8 || spec.stencil_pass < 1 || spec.stencil_pass > 8 || spec.stencil_read_mask > 255 || spec.stencil_write_mask > 255))) { throw std::runtime_error("invalid stencil pipeline state"); }
      auto shader = [](std::span<const uint8_t> code) {
        auto source = WGPU_SHADER_SOURCE_WGSL_INIT;
        source.code = {reinterpret_cast<const char*>(code.data()), code.size()};
        auto descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; descriptor.nextInChain = &source.chain;
        return wgpuDeviceCreateShaderModule(device, &descriptor);
      };
      auto vertex = shader(bytes.first(spec.vertex_bytes)); auto fragment = shader(bytes.subspan(spec.vertex_bytes));
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
      auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = WGPUTextureFormat_RGBA8Unorm; target.writeMask = spec.write_mask;
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
      descriptor.layout = (spec.cube_mask || spec.shadow_mask || spec.volume_mask) ? custom.second : layout; descriptor.vertex.module = vertex; descriptor.vertex.entryPoint = text("vs_main"); descriptor.vertex.bufferCount = 1; descriptor.vertex.buffers = &vertex_layout;
      descriptor.fragment = &pixel; descriptor.primitive.topology = static_cast<WGPUPrimitiveTopology>(spec.topology); descriptor.primitive.cullMode = static_cast<WGPUCullMode>(spec.cull); descriptor.primitive.frontFace = static_cast<WGPUFrontFace>(spec.front_face);
      if (spec.depth) { descriptor.depthStencil = &depth; }
      auto pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor); pipelines.emplace(resource, Pipeline{pipeline, attributeCount * 16}); created_resource = resource;
      if (spec.cube_mask || spec.shadow_mask || spec.volume_mask) { wgpuPipelineLayoutRelease(custom.second); wgpuBindGroupLayoutRelease(custom.first); }
      wgpuShaderModuleRelease(vertex); wgpuShaderModuleRelease(fragment);
      break;
    }
    case Opcode::Draw: { draw(resource, bytes); break; }
    case Opcode::DrawBatch:
    case Opcode::RenderBatch: {
      const bool mixed = static_cast<Opcode>(operation) == Opcode::RenderBatch;
      if (resource) { throw std::runtime_error("draw batch cannot name a target"); }
      const uint32_t count = packet<uint32_t>(bytes);
      if (!count || count > 256) { throw std::runtime_error("invalid draw batch count"); }
      DrawUploads uploads;
      uploads.prepare(bytes, count, mixed);
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
      break;
    }
    case Opcode::Clear: { clear(resource, bytes); break; }
    case Opcode::UploadDepth: {
      const auto spec = packet<DepthUploadPacket>(bytes);
      auto& target = texture(resource);
      if (!target.depth || spec.width != target.width || spec.height != target.height ||
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
      auto values = buffer(bytes, WGPUBufferUsage_Storage);
      const std::array<uint32_t, 4> parameters{spec.width, spec.depth_bits == 24 ? 16777215u : 65535u, 0, 0};
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
      auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr); auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
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
    case Opcode::SetGamma: {
      exact(bytes, gamma_ramp.size()); std::memcpy(gamma_ramp.data(), bytes.data(), gamma_ramp.size()); upload_gamma(); break;
    }
    case Opcode::Present: {
      auto present = packet<PresentPacket>(bytes); exact(bytes, 0);
      auto& source = texture(resource);
      if (source.depth || source.cube || source.volumeDepth > 1 || present.pillarbox > 1) { throw std::runtime_error("unsupported presentation source or flags"); }
      prepare_presentation();
      auto current = WGPU_SURFACE_TEXTURE_INIT; wgpuSurfaceGetCurrentTexture(surface, &current);
      if (!current.texture) { throw std::runtime_error("canvas surface unavailable"); }
      auto source_view = wgpuTextureCreateView(source.handle, nullptr); auto ramp_view = wgpuTextureCreateView(gamma_texture, nullptr); auto target_view = wgpuTextureCreateView(presentation_target.handle, nullptr);
      std::array<WGPUBindGroupEntry, 2> entries{};
      entries[0] = WGPU_BIND_GROUP_ENTRY_INIT; entries[0].binding = 0; entries[0].textureView = source_view;
      entries[1] = WGPU_BIND_GROUP_ENTRY_INIT; entries[1].binding = 1; entries[1].textureView = ramp_view;
      auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = presentation_bindings; group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data(); auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
      auto color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; color.view = target_view; color.loadOp = WGPULoadOp_Clear; color.storeOp = WGPUStoreOp_Store; color.clearValue = {0, 0, 0, 1};
      auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &color;
      auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr); auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
      float width = canvas_width, height = canvas_height;
      if (present.pillarbox) { if (width * 3 > height * 4) { width = height * 4 / 3; } else { height = width * 3 / 4; } }
      wgpuRenderPassEncoderSetViewport(pass, (canvas_width - width) / 2, (canvas_height - height) / 2, width, height, 0, 1);
      wgpuRenderPassEncoderSetPipeline(pass, presentation_pipeline); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr); wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0); wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
      auto from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; from.texture = presentation_target.handle; auto to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; to.texture = current.texture; WGPUExtent3D extent{canvas_width, canvas_height, 1}; wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
      submit(encoder);
      wgpuBindGroupRelease(group); wgpuTextureViewRelease(source_view); wgpuTextureViewRelease(ramp_view); wgpuTextureViewRelease(target_view); wgpuTextureRelease(current.texture);
      auto info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT; info.mode = WGPUCallbackMode_AllowSpontaneous;
      info.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView message, void*, void*) { if (status != WGPUQueueWorkDoneStatus_Success) { failure = string(message); } if (active && device) { finish(); } };
      wgpuQueueOnSubmittedWorkDone(queue, info); return;
    }
    case Opcode::Readback: {
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
      struct Read { WGPUBuffer buffer; WGPUTexture packed; uint32_t width, height, pitch, pixel_bytes, stencil_pitch, map_bytes; };
      auto read = new Read{staging, packed, target.width, target.height, pitch, pixel_bytes, stencilPitch, mapBytes};
      auto info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT; info.mode = WGPUCallbackMode_AllowSpontaneous; info.userdata1 = read;
      info.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* userdata, void*) {
        auto read = static_cast<Read*>(userdata);
        if (status == WGPUMapAsyncStatus_Success) {
          auto mapped = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(read->buffer, 0, read->map_bytes));
          reply.resize(read->width * read->height * read->pixel_bytes);
          for (uint32_t n = 0; n < read->height; ++n) {
            auto row = reply.data() + n * read->width * read->pixel_bytes;
            std::memcpy(row, mapped + n * read->pitch, read->width * read->pixel_bytes);
            if (read->stencil_pitch) {
              const auto stencil = mapped + read->pitch * read->height + n * read->stencil_pitch;
              for (uint32_t n = 0; n < read->width; ++n) { row[n * 4] = stencil[n]; }
            }
          }
          wgpuBufferUnmap(read->buffer);
        }
        else { failure = string(message); }
        wgpuBufferDestroy(read->buffer); wgpuBufferRelease(read->buffer);
        if (read->packed) { wgpuTextureDestroy(read->packed); wgpuTextureRelease(read->packed); }
        delete read;
        if (active && device) { finish(); }
      };
      wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, descriptor.size, info); return;
    }
    case Opcode::Destroy: {
      exact(bytes, 0);
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
      opcode == Opcode::Clear || opcode == Opcode::Copy || opcode == Opcode::Destroy)) {
    defer_validation(command, resource);
    return;
  }
  finish();
}
}
