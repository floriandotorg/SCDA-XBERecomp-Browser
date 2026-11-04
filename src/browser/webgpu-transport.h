#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace browser::webgpu {
constexpr uint32_t MaxBytes = 4 * 1024 * 1024;
enum class Opcode : uint32_t { Configure = 1, CreateTexture = 2, CreatePipeline = 3, Draw = 4, Readback = 5, Destroy = 6, Flush = 7, Shutdown = 8, Present = 9, Clear = 10, Copy = 11, SetGamma = 12, UploadDepth = 13, DrawBatch = 14, RenderBatch = 15 };
enum class Status : uint32_t { Ok, Error, Closed, Lost };
struct Dimensions { uint32_t width, height; };
struct TexturePacket { uint32_t width, height, depth; uint32_t levels = 1; };
struct PipelinePacket {
  uint32_t vertex_bytes, fragment_bytes;
  uint32_t topology, cull, front_face, blend, src_color, dst_color, color_op, src_alpha, dst_alpha, alpha_op, write_mask;
  uint32_t depth, depth_write, depth_compare;
  uint32_t stencil, stencil_compare, stencil_fail, stencil_depth_fail, stencil_pass, stencil_read_mask, stencil_write_mask;
  uint32_t cube_mask = 0, color_target = 1, shadow_mask = 0, vertex_mask = 0, volume_mask = 0;
};
struct DrawPacket {
  uint32_t pipeline, texture[4], depth_target, vertex_count, index_count, index32, vertex_bytes, index_bytes, uniform_bytes;
  float viewport[6];
  uint32_t scissor[4];
  uint32_t sampler[4][7];
  uint32_t stencil_reference;
  float blend_constant[4];
  uint32_t address_w[4] = {1, 1, 1, 1};
};
struct DepthUploadPacket { uint32_t width, height, depth_bits, stencil_mask[8]; };
static_assert(sizeof(DepthUploadPacket) == 44);
struct ClearPacket { float color[4], depth; uint32_t stencil, flags, depth_target; uint32_t rect_count = 0; };
struct PresentPacket { uint32_t pillarbox; };
struct CopyPacket { uint32_t source, width, height; uint32_t source_x = 0, source_y = 0, destination_x = 0, destination_y = 0, force_opaque = 0; };
static_assert(sizeof(CopyPacket) == 32);
static_assert(sizeof(PipelinePacket) == 112);
static_assert(sizeof(DrawPacket) == 236);
static_assert(sizeof(ClearPacket) == 36);
struct Result { Status status; uint32_t command, resource; std::vector<uint8_t> bytes; };
class Transport {
public:
  virtual ~Transport() = default;
  virtual Result Execute(Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes = {});
  virtual Result Post(Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes = {});
};
}
