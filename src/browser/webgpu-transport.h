#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace browser::webgpu {
constexpr uint32_t MaxBytes = 4 * 1024 * 1024;
constexpr uint32_t ReadbackSlots = 4;
enum class Opcode : uint32_t { Configure = 1, CreateTexture = 2, CreatePipeline = 3, Draw = 4, Readback = 5, Destroy = 6, Flush = 7, Shutdown = 8, Present = 9, Clear = 10, Copy = 11, SetGamma = 12, UploadDepth = 13, DrawBatch = 14, RenderBatch = 15, UploadBuffer = 16, UploadDrawBatch = 17, ReadbackStart = 18, ReadbackCollect = 19, Blit = 20, Multisample = 21, SoftShadows = 22, ApplyShadows = 23 };
enum class Status : uint32_t { Ok, Error, Closed, Lost };
struct Dimensions { uint32_t width, height; };
struct TexturePacket { uint32_t width, height, depth; uint32_t levels = 1; };
struct PipelinePacket {
  uint32_t vertex_bytes, fragment_bytes;
  uint32_t topology, cull, front_face, blend, src_color, dst_color, color_op, src_alpha, dst_alpha, alpha_op, write_mask;
  uint32_t depth, depth_write, depth_compare;
  uint32_t stencil, stencil_compare, stencil_fail, stencil_depth_fail, stencil_pass, stencil_read_mask, stencil_write_mask;
  uint32_t cube_mask = 0, color_target = 1, shadow_mask = 0, vertex_mask = 0, volume_mask = 0;
  uint32_t sample_count = 1;  // 4 when the draw's targets have multisampled companions
};
struct DrawPacket {
  uint32_t pipeline, texture[4], depth_target, vertex_count, index_count, index32, vertex_bytes, index_bytes, uniform_bytes;
  float viewport[6];
  uint32_t scissor[4];
  uint32_t sampler[4][7];
  uint32_t stencil_reference;
  float blend_constant[4];
  uint32_t address_w[4] = {1, 1, 1, 1};
  uint32_t anisotropy[4] = {1, 1, 1, 1};  // maxAnisotropy a stage, 1 to 16; above 1 every filter is linear
};
constexpr uint32_t ResidentVertexMask = 0x10000;
struct RawAttribute { uint32_t stream, offset, format; };
struct RawLayout { uint32_t strides[16]{}; RawAttribute attributes[16]{}; };
struct UploadDrawPacket { uint32_t upload_count, upload_bytes, draw_bytes, mixed; };
struct BufferUpload { uint32_t offset, capacity; };
struct ResidentDraw { uint32_t vertices[16]{}, sizes[16]{}, offsets[16]{}, indices = 0, index_bytes = 0, index_offset = 0; };
static_assert(sizeof(UploadDrawPacket) == 16);
static_assert(sizeof(BufferUpload) == 8 && sizeof(ResidentDraw) == 204 && sizeof(RawLayout) == 256);
struct DepthUploadPacket { uint32_t width, height, depth_bits, stencil_mask[8]; };
static_assert(sizeof(DepthUploadPacket) == 44);
struct ClearPacket { float color[4], depth; uint32_t stencil, flags, depth_target; uint32_t rect_count = 0; };
// pillarbox: PresentPillarbox shows a 4:3 picture in the middle, PresentLinear samples the frame bilinearly
enum : uint32_t { PresentPillarbox = 1, PresentLinear = 2, PresentSmaa = 4 };
struct PresentPacket { uint32_t pillarbox; };
// a source rectangle drawn over a destination rectangle (the resource): colour by box filtering taps x taps source
// texels a pixel (bilinear for 0), depth by the nearest texel; force_opaque writes alpha 1 (X8 formats)
struct BlitPacket {
  uint32_t source;
  float source_rect[4], destination_rect[4];
  uint32_t taps = 0, force_opaque = 0;
};
static_assert(sizeof(BlitPacket) == 44);
// Chaos Theory's soft shadows over the scene (the resource): its depth, view z = zp.x / (depth - zp.y), the scene's
// viewport in host pixels (vp), the copy's and the mask's sizes (sz.xy); the filtered shadow waits for ApplyShadows
struct SoftShadowPacket {
  uint32_t depth;
  float zp[4], vp[4], sz[4];
};
static_assert(sizeof(SoftShadowPacket) == 52);
struct CopyPacket { uint32_t source, width, height; uint32_t source_x = 0, source_y = 0, destination_x = 0, destination_y = 0, force_opaque = 0; };
static_assert(sizeof(CopyPacket) == 32);
static_assert(sizeof(PipelinePacket) == 116);
static_assert(sizeof(DrawPacket) == 252);
static_assert(sizeof(ClearPacket) == 36);
struct Result { Status status; uint32_t command, resource; std::vector<uint8_t> bytes; };
class Transport {
public:
  virtual ~Transport() = default;
  virtual Result Execute(Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes = {});
  virtual Result Post(Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes = {});
};
}
