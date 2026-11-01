#include "webgpu-transport.h"
#include <webgpu/webgpu.h>
#include <emscripten.h>
#include <array>
#include <cstring>
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
WGPUSurface surface;
WGPUBindGroupLayout bindings;
WGPUPipelineLayout layout;
WGPUSampler sampler;
struct Texture { WGPUTexture handle; uint32_t width, height; bool depth; };
std::map<uint32_t, Texture> textures;
std::map<uint32_t, WGPURenderPipeline> pipelines;
std::vector<uint8_t> reply;
std::string failure;
bool closed = false;
bool active = false;
uint32_t created_resource = 0;
uint64_t texture_bytes = 0;
constexpr uint64_t TextureBudget = 256ull * 1024 * 1024;
uint32_t canvas_width = 0, canvas_height = 0;
WGPUTexture gamma_texture;
WGPURenderPipeline presentation_pipeline;
WGPUBindGroupLayout presentation_bindings;
WGPUPipelineLayout presentation_layout;
Texture presentation_target{};
std::array<uint8_t, 768> gamma_ramp = [] {
  std::array<uint8_t, 768> result{};
  for (uint32_t n = 0; n < result.size(); ++n) { result[n] = n % 256; }
  return result;
}();
void discard(uint32_t id) {
  auto image = textures.find(id);
  if (image != textures.end()) { texture_bytes -= uint64_t(image->second.width) * image->second.height * 4; wgpuTextureDestroy(image->second.handle); wgpuTextureRelease(image->second.handle); textures.erase(image); }
  auto pipeline = pipelines.find(id);
  if (pipeline != pipelines.end()) { wgpuRenderPipelineRelease(pipeline->second); pipelines.erase(pipeline); }
}
WGPUStringView text(const char* value) { return {value, std::strlen(value)}; }
EM_JS(void, ready, (int status, const char* message), { Module.onGPUReady(status, UTF8ToString(message)); });
EM_JS(void, adapter_details, (const char* vendor, const char* architecture, const char* description, const uint32_t* features, uint32_t count, uint32_t max_texture, uint32_t max_attributes, uint32_t max_stride, uint32_t max_uniform, uint32_t compression), {
  Module.onGPUAdapter?.({ vendor: UTF8ToString(vendor), architecture: UTF8ToString(architecture), description: UTF8ToString(description), features: Array.from(HEAPU32.subarray(features >> 2, (features >> 2) + count)), textureCompressionBC: Boolean(compression & 1), textureCompressionETC2: Boolean(compression & 2), textureCompressionASTC: Boolean(compression & 4), limits: { maxTextureDimension2D: max_texture, maxVertexAttributes: max_attributes, maxVertexBufferArrayStride: max_stride, maxUniformBufferBindingSize: max_uniform }, source: 'service-adapter' });
});
EM_JS(void, completed, (int status, const uint8_t* bytes, uint32_t size, const char* message), { Module.onGPUComplete(status, HEAPU8.slice(bytes, bytes + size), UTF8ToString(message)); });
EM_JS(void, uncaptured, (const char* message), { Module.onGPUError(UTF8ToString(message)); });
EM_JS(void, lost, (const char* message), { Module.onGPULost(UTF8ToString(message)); });
EM_JS(void, register_canvas, (), { specialHTMLTargets['#canvas'] = Module.canvas; });
EM_JS(void, resize_canvas, (uint32_t width, uint32_t height), { Module.canvas.width = width; Module.canvas.height = height; });
std::string string(WGPUStringView value) { return std::string(value.data ? value.data : "", value.data ? value.length : 0); }
void finish() {
  auto info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
  info.mode = WGPUCallbackMode_AllowSpontaneous;
  info.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void*, void*) {
    if (type != WGPUErrorType_NoError || status != WGPUPopErrorScopeStatus_Success) { failure = string(message); }
    if (!active) { return; }
    active = false;
    if (!failure.empty() && created_resource) { discard(created_resource); }
    created_resource = 0;
    completed(failure.empty() ? 0 : 1, reply.data(), reply.size(), failure.c_str());
  };
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
void release_resources() {
  release_presentation_target();
  if (gamma_texture) { wgpuTextureDestroy(gamma_texture); wgpuTextureRelease(gamma_texture); gamma_texture = nullptr; }
  if (presentation_pipeline) { wgpuRenderPipelineRelease(presentation_pipeline); presentation_pipeline = nullptr; }
  if (presentation_layout) { wgpuPipelineLayoutRelease(presentation_layout); presentation_layout = nullptr; }
  if (presentation_bindings) { wgpuBindGroupLayoutRelease(presentation_bindings); presentation_bindings = nullptr; }
  for (auto& entry : textures) { wgpuTextureDestroy(entry.second.handle); wgpuTextureRelease(entry.second.handle); }
  textures.clear(); texture_bytes = 0;
  for (auto& entry : pipelines) { wgpuRenderPipelineRelease(entry.second); }
  pipelines.clear();
}
void initialize_bindings() {
  std::array<WGPUBindGroupLayoutEntry, 9> entries{};
  for (uint32_t n = 0; n < entries.size(); ++n) {
    entries[n] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[n].binding = n;
    entries[n].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    if (n == 0) { entries[n].buffer.type = WGPUBufferBindingType_Uniform; }
    else if (n % 2 == 1) { entries[n].texture.sampleType = WGPUTextureSampleType_Float; entries[n].texture.viewDimension = WGPUTextureViewDimension_2D; }
    else { entries[n].sampler.type = WGPUSamplerBindingType_Filtering; }
  }
  auto descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  descriptor.entryCount = entries.size(); descriptor.entries = entries.data();
  bindings = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
  auto pipeline = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  pipeline.bindGroupLayoutCount = 1; pipeline.bindGroupLayouts = &bindings;
  layout = wgpuDeviceCreatePipelineLayout(device, &pipeline);
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
EMSCRIPTEN_KEEPALIVE uint32_t webgpu_resources() { return textures.size() + pipelines.size() + (gamma_texture ? 1 : 0) + (presentation_pipeline ? 1 : 0) + (presentation_target.handle ? 1 : 0); }
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
EMSCRIPTEN_KEEPALIVE void webgpu_submit(uint32_t operation, uint32_t resource, const uint8_t* data, uint32_t size) {
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
      if (!spec.width || !spec.height || spec.width > 8192 || spec.height > 8192 || spec.depth > 1) { throw std::runtime_error("invalid texture dimensions or format"); }
      exact(bytes, spec.depth ? 0 : uint64_t(spec.width) * spec.height * 4);
      if (texture_bytes + uint64_t(spec.width) * spec.height * 4 > TextureBudget) { throw std::runtime_error("GPU texture budget exhausted"); }
      auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
      descriptor.size = {spec.width, spec.height, 1};
      descriptor.format = spec.depth ? WGPUTextureFormat_Depth24PlusStencil8 : WGPUTextureFormat_RGBA8Unorm;
      descriptor.usage = WGPUTextureUsage_RenderAttachment;
      if (!spec.depth) { descriptor.usage |= WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst; }
      auto handle = wgpuDeviceCreateTexture(device, &descriptor);
      textures.emplace(resource, Texture{handle, spec.width, spec.height, spec.depth != 0}); texture_bytes += uint64_t(spec.width) * spec.height * 4; created_resource = resource;
      if (!spec.depth) {
        auto destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; destination.texture = handle;
        auto layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT; layout.bytesPerRow = spec.width * 4; layout.rowsPerImage = spec.height;
        wgpuQueueWriteTexture(queue, &destination, bytes.data(), bytes.size(), &layout, &descriptor.size);
      }
      break;
    }
    case Opcode::CreatePipeline: {
      if (!resource || textures.contains(resource) || pipelines.contains(resource) || textures.size() + pipelines.size() >= 4096) { throw std::runtime_error("duplicate or exhausted GPU resource ID"); }
      if (pipelines.size() >= 256) { throw std::runtime_error("GPU pipeline budget exhausted"); }
      auto spec = packet<PipelinePacket>(bytes); exact(bytes, uint64_t(spec.vertex_bytes) + spec.fragment_bytes);
      auto shader = [](std::span<const uint8_t> code) {
        auto source = WGPU_SHADER_SOURCE_WGSL_INIT;
        source.code = {reinterpret_cast<const char*>(code.data()), code.size()};
        auto descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; descriptor.nextInChain = &source.chain;
        return wgpuDeviceCreateShaderModule(device, &descriptor);
      };
      auto vertex = shader(bytes.first(spec.vertex_bytes)); auto fragment = shader(bytes.subspan(spec.vertex_bytes));
      std::array<WGPUVertexAttribute, 16> attributes{};
      for (uint32_t n = 0; n < attributes.size(); ++n) { attributes[n] = WGPU_VERTEX_ATTRIBUTE_INIT; attributes[n].format = WGPUVertexFormat_Float32x4; attributes[n].offset = n * 16; attributes[n].shaderLocation = n; }
      auto vertex_layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT; vertex_layout.arrayStride = 256; vertex_layout.attributeCount = attributes.size(); vertex_layout.attributes = attributes.data();
      auto target = WGPU_COLOR_TARGET_STATE_INIT; target.format = WGPUTextureFormat_RGBA8Unorm; target.writeMask = spec.write_mask;
      auto blend = WGPU_BLEND_STATE_INIT;
      blend.color.srcFactor = static_cast<WGPUBlendFactor>(spec.src_color); blend.color.dstFactor = static_cast<WGPUBlendFactor>(spec.dst_color); blend.color.operation = static_cast<WGPUBlendOperation>(spec.color_op);
      blend.alpha.srcFactor = static_cast<WGPUBlendFactor>(spec.src_alpha); blend.alpha.dstFactor = static_cast<WGPUBlendFactor>(spec.dst_alpha); blend.alpha.operation = static_cast<WGPUBlendOperation>(spec.alpha_op);
      if (spec.blend) { target.blend = &blend; }
      auto pixel = WGPU_FRAGMENT_STATE_INIT; pixel.module = fragment; pixel.entryPoint = text("fs_main"); pixel.targetCount = 1; pixel.targets = &target;
      auto depth = WGPU_DEPTH_STENCIL_STATE_INIT; depth.format = WGPUTextureFormat_Depth24PlusStencil8; depth.depthWriteEnabled = spec.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False; depth.depthCompare = static_cast<WGPUCompareFunction>(spec.depth_compare);
      auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
      descriptor.layout = layout; descriptor.vertex.module = vertex; descriptor.vertex.entryPoint = text("vs_main"); descriptor.vertex.bufferCount = 1; descriptor.vertex.buffers = &vertex_layout;
      descriptor.fragment = &pixel; descriptor.primitive.topology = static_cast<WGPUPrimitiveTopology>(spec.topology); descriptor.primitive.cullMode = static_cast<WGPUCullMode>(spec.cull); descriptor.primitive.frontFace = static_cast<WGPUFrontFace>(spec.front_face);
      if (spec.depth) { descriptor.depthStencil = &depth; }
      auto pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor); pipelines.emplace(resource, pipeline); created_resource = resource;
      wgpuShaderModuleRelease(vertex); wgpuShaderModuleRelease(fragment);
      break;
    }
    case Opcode::Draw: {
      auto spec = packet<DrawPacket>(bytes); exact(bytes, uint64_t(spec.vertex_bytes) + spec.index_bytes + spec.uniform_bytes);
      auto& target = texture(resource);
      if (target.depth || !spec.vertex_bytes || spec.vertex_bytes % 256 || spec.vertex_count > spec.vertex_bytes / 256 || spec.index32 > 1 || spec.index_bytes != uint64_t(spec.index_count) * (spec.index32 ? 4 : 2) || !spec.uniform_bytes || spec.uniform_bytes > 65536 || spec.uniform_bytes % 16) { throw std::runtime_error("invalid draw payload"); }
      for (uint32_t n = 0; n < spec.index_count; ++n) {
        uint32_t index = 0;
        std::memcpy(&index, bytes.data() + spec.vertex_bytes + n * (spec.index32 ? 4 : 2), spec.index32 ? 4 : 2);
        if (index >= spec.vertex_bytes / 256) { throw std::runtime_error("draw index exceeds owned vertices"); }
      }
      auto found = pipelines.find(spec.pipeline); if (found == pipelines.end()) { throw std::runtime_error("unknown pipeline resource"); }
      for (uint32_t n = 0; n < 4; ++n) {
        if (texture(spec.texture[n]).depth || spec.texture[n] == resource) { throw std::runtime_error("invalid sampled texture resource"); }
        if (spec.sampler[n][0] < 1 || spec.sampler[n][0] > 3 || spec.sampler[n][1] < 1 || spec.sampler[n][1] > 3 || spec.sampler[n][2] < 1 || spec.sampler[n][2] > 2 || spec.sampler[n][3] < 1 || spec.sampler[n][3] > 2 || spec.sampler[n][4] < 1 || spec.sampler[n][4] > 2) { throw std::runtime_error("unsupported sampler state"); }
      }
      if (spec.depth_target && !texture(spec.depth_target).depth) { throw std::runtime_error("invalid depth target resource"); }
      auto vertices = buffer(bytes.first(spec.vertex_bytes), WGPUBufferUsage_Vertex);
      auto uniforms = buffer(bytes.subspan(spec.vertex_bytes + spec.index_bytes), WGPUBufferUsage_Uniform);
      WGPUBuffer indices = nullptr;
      if (spec.index_count) { indices = buffer(bytes.subspan(spec.vertex_bytes, spec.index_bytes), WGPUBufferUsage_Index); }
      std::array<WGPUBindGroupEntry, 9> entries{}; std::array<WGPUTextureView, 4> views{}; std::array<WGPUSampler, 4> samplers{};
      entries[0] = WGPU_BIND_GROUP_ENTRY_INIT; entries[0].binding = 0; entries[0].buffer = uniforms; entries[0].size = spec.uniform_bytes;
      for (uint32_t n = 0; n < 4; ++n) {
        auto& sampled = texture(spec.texture[n]);
        auto sampling = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sampling.addressModeU = static_cast<WGPUAddressMode>(spec.sampler[n][0]); sampling.addressModeV = static_cast<WGPUAddressMode>(spec.sampler[n][1]);
        sampling.magFilter = static_cast<WGPUFilterMode>(spec.sampler[n][2]); sampling.minFilter = static_cast<WGPUFilterMode>(spec.sampler[n][3]); sampling.mipmapFilter = static_cast<WGPUMipmapFilterMode>(spec.sampler[n][4]);
        samplers[n] = wgpuDeviceCreateSampler(device, &sampling);
        views[n] = wgpuTextureCreateView(sampled.handle, nullptr);
        entries[1 + n * 2] = WGPU_BIND_GROUP_ENTRY_INIT; entries[1 + n * 2].binding = 1 + n * 2; entries[1 + n * 2].textureView = views[n];
        entries[2 + n * 2] = WGPU_BIND_GROUP_ENTRY_INIT; entries[2 + n * 2].binding = 2 + n * 2; entries[2 + n * 2].sampler = samplers[n];
      }
      auto group_descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT; group_descriptor.layout = bindings; group_descriptor.entryCount = entries.size(); group_descriptor.entries = entries.data();
      auto group = wgpuDeviceCreateBindGroup(device, &group_descriptor);
      auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; attachment.view = wgpuTextureCreateView(target.handle, nullptr); attachment.loadOp = WGPULoadOp_Load; attachment.storeOp = WGPUStoreOp_Store;
      auto pass_descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT; pass_descriptor.colorAttachmentCount = 1; pass_descriptor.colorAttachments = &attachment;
      auto depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
      if (spec.depth_target) { depth.depthClearValue = 1; depth.view = wgpuTextureCreateView(texture(spec.depth_target).handle, nullptr); depth.depthLoadOp = WGPULoadOp_Load; depth.depthStoreOp = WGPUStoreOp_Store; depth.stencilLoadOp = WGPULoadOp_Load; depth.stencilStoreOp = WGPUStoreOp_Store; pass_descriptor.depthStencilAttachment = &depth; }
      auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr); auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
      wgpuRenderPassEncoderSetPipeline(pass, found->second); wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr); wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertices, 0, spec.vertex_bytes);
      wgpuRenderPassEncoderSetViewport(pass, spec.viewport[0], spec.viewport[1], spec.viewport[2], spec.viewport[3], spec.viewport[4], spec.viewport[5]); wgpuRenderPassEncoderSetScissorRect(pass, spec.scissor[0], spec.scissor[1], spec.scissor[2], spec.scissor[3]);
      if (indices) { wgpuRenderPassEncoderSetIndexBuffer(pass, indices, spec.index32 ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16, 0, spec.index_bytes); wgpuRenderPassEncoderDrawIndexed(pass, spec.index_count, 1, 0, 0, 0); }
      else { wgpuRenderPassEncoderDraw(pass, spec.vertex_count, 1, 0, 0); }
      wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass); submit(encoder);
      wgpuTextureViewRelease(attachment.view); if (depth.view) { wgpuTextureViewRelease(depth.view); }
      for (auto view : views) { wgpuTextureViewRelease(view); }
      for (auto item : samplers) { wgpuSamplerRelease(item); }
      wgpuBindGroupRelease(group); wgpuBufferRelease(vertices); wgpuBufferRelease(uniforms); if (indices) { wgpuBufferRelease(indices); }
      break;
    }
    case Opcode::Clear: {
      auto spec = packet<ClearPacket>(bytes); exact(bytes, 0);
      auto color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT; auto depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
      auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
      if (spec.flags & 1) { color.view = wgpuTextureCreateView(texture(resource).handle, nullptr); color.loadOp = WGPULoadOp_Clear; color.storeOp = WGPUStoreOp_Store; color.clearValue = {spec.color[0], spec.color[1], spec.color[2], spec.color[3]}; descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &color; }
      if (spec.flags & 6) { depth.view = wgpuTextureCreateView(texture(spec.depth_target).handle, nullptr); depth.depthLoadOp = spec.flags & 2 ? WGPULoadOp_Clear : WGPULoadOp_Load; depth.depthStoreOp = WGPUStoreOp_Store; depth.depthClearValue = spec.depth; depth.stencilLoadOp = spec.flags & 4 ? WGPULoadOp_Clear : WGPULoadOp_Load; depth.stencilStoreOp = WGPUStoreOp_Store; depth.stencilClearValue = spec.stencil; descriptor.depthStencilAttachment = &depth; }
      auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr); auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor); wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass); submit(encoder);
      if (color.view) { wgpuTextureViewRelease(color.view); } if (depth.view) { wgpuTextureViewRelease(depth.view); }
      break;
    }
    case Opcode::Copy: {
      auto copy = packet<CopyPacket>(bytes); exact(bytes, 0);
      auto& source = texture(copy.source); auto& target = texture(resource);
      if (source.depth || target.depth || copy.width != source.width || copy.height != source.height || source.width != target.width || source.height != target.height || copy.source == resource) { throw std::runtime_error("unsupported partial, depth or aliased texture copy"); }
      auto from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; from.texture = source.handle; auto to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; to.texture = target.handle;
      WGPUExtent3D extent{source.width, source.height, 1}; auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr); wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent); submit(encoder);
      break;
    }
    case Opcode::SetGamma: {
      exact(bytes, gamma_ramp.size()); std::memcpy(gamma_ramp.data(), bytes.data(), gamma_ramp.size()); upload_gamma(); break;
    }
    case Opcode::Present: {
      auto present = packet<PresentPacket>(bytes); exact(bytes, 0);
      auto& source = texture(resource);
      if (source.depth || present.pillarbox > 1) { throw std::runtime_error("unsupported presentation source or flags"); }
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
      auto from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; from.texture = presentation_target.handle; auto to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; to.texture = current.texture; WGPUExtent3D extent{canvas_width, canvas_height, 1}; wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent); submit(encoder);
      wgpuBindGroupRelease(group); wgpuTextureViewRelease(source_view); wgpuTextureViewRelease(ramp_view); wgpuTextureViewRelease(target_view); wgpuTextureRelease(current.texture);
      auto info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT; info.mode = WGPUCallbackMode_AllowSpontaneous;
      info.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView message, void*, void*) { if (status != WGPUQueueWorkDoneStatus_Success) { failure = string(message); } if (active && device) { finish(); } };
      wgpuQueueOnSubmittedWorkDone(queue, info); return;
    }
    case Opcode::Readback: {
      exact(bytes, 0); auto target = resource ? texture(resource) : presentation_target;
      if (!target.handle) { throw std::runtime_error("no completed presentation target"); }
      const uint32_t pitch = (target.width * 4 + 255) & ~255u;
      if (target.depth || uint64_t(pitch) * target.height > MaxBytes) { throw std::runtime_error("readback exceeds bounded capacity or uses depth"); }
      auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT; descriptor.size = pitch * target.height; descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
      auto staging = wgpuDeviceCreateBuffer(device, &descriptor);
      auto source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; source.texture = target.handle;
      auto destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT; destination.buffer = staging; destination.layout.bytesPerRow = pitch; destination.layout.rowsPerImage = target.height;
      WGPUExtent3D extent{target.width, target.height, 1}; auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr); wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent); submit(encoder);
      struct Read { WGPUBuffer buffer; uint32_t width, height, pitch; }; auto read = new Read{staging, target.width, target.height, pitch};
      auto info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT; info.mode = WGPUCallbackMode_AllowSpontaneous; info.userdata1 = read;
      info.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* userdata, void*) {
        auto read = static_cast<Read*>(userdata);
        if (status == WGPUMapAsyncStatus_Success) { auto mapped = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(read->buffer, 0, read->pitch * read->height)); reply.resize(read->width * read->height * 4); for (uint32_t n = 0; n < read->height; ++n) { std::memcpy(reply.data() + n * read->width * 4, mapped + n * read->pitch, read->width * 4); } wgpuBufferUnmap(read->buffer); }
        else { failure = string(message); }
        wgpuBufferDestroy(read->buffer); wgpuBufferRelease(read->buffer); delete read;
        if (active && device) { finish(); }
      };
      wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, descriptor.size, info); return;
    }
    case Opcode::Destroy: {
      exact(bytes, 0); auto item = textures.find(resource);
      if (item != textures.end()) { texture_bytes -= uint64_t(item->second.width) * item->second.height * 4; wgpuTextureDestroy(item->second.handle); wgpuTextureRelease(item->second.handle); textures.erase(item); }
      else { auto pipeline = pipelines.find(resource); if (pipeline == pipelines.end()) { throw std::runtime_error("unknown GPU resource"); } wgpuRenderPipelineRelease(pipeline->second); pipelines.erase(pipeline); }
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
  finish();
}
}
