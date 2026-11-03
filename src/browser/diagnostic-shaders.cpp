#include "browser/diagnostic-shaders.h"
#include <algorithm>

namespace browser {
namespace {

constexpr uint32_t declarationWordLimit = 1024;
constexpr uint32_t instructionLimit = 136;

class ShaderWords {
  Memory& memory_;
  const X86Context& ctx_;
  uint64_t address_;
  uint32_t remaining_;
public:
  ShaderWords(Memory& memory, const X86Context& ctx, uint32_t address, uint32_t remaining)
    : memory_(memory), ctx_(ctx), address_(address), remaining_(remaining) {}
  void Read(uint32_t* output, uint32_t count) {
    if (count > remaining_) { throw Fault(ctx_, uint32_t(address_), size_t(count) * 4, "guest shader word budget exceeded"); }
    if (address_ >= (1ull << 32) || uint64_t(count) * 4 > (1ull << 32) - address_) {
      throw Fault(ctx_, uint32_t(address_), size_t(count) * 4, "guest shader address wraparound");
    }
    memory_.Read(ctx_, uint32_t(address_), output, size_t(count) * 4);
    address_ += uint64_t(count) * 4;
    remaining_ -= count;
  }
  uint32_t Word() {
    uint32_t value = 0;
    Read(&value, 1);
    return value;
  }
};

uint32_t TypeBytes(const X86Context& ctx, uint32_t address, uint32_t type) {
  switch (type) {
    case 0x02: return 0;
    case 0x12: case 0x40: case 0x21: case 0x44: case 0x25: case 0x16: return 4;
    case 0x22: case 0x41: case 0x45: return 8;
    case 0x32: case 0x72: return 12;
    case 0x42: return 16;
    case 0x11: case 0x24: case 0x15: return 2;
    case 0x31: case 0x35: return 6;
    case 0x14: return 1;
    case 0x34: return 3;
    default: throw Fault(ctx, address, 4, "unsupported guest vertex declaration type");
  }
}

void Declaration(Memory& memory, const X86Context& ctx, uint32_t address, gpu::VertexShader& shader) {
  ShaderWords words(memory, ctx, address, declarationWordLimit);
  uint32_t stream = 0;
  uint32_t offset = 0;
  for (;;) {
    const uint32_t tokenAddress = address + uint32_t(shader.decl.size()) * 4;
    const uint32_t token = words.Word();
    shader.decl.push_back(token);
    if (token == 0xffffffff) { break; }
    const uint32_t kind = token >> 29;
    if (kind == 0 && token == 0) { continue; }
    if (kind == 1 && (token & ~0x2000000fu) == 0) {
      shader.stream_stride[stream] = std::max(shader.stream_stride[stream], offset);
      stream = token & 15;
      offset = shader.stream_stride[stream];
    } else if (kind == 2 && (token & (1u << 28)) && (token & ~0x58ff0000u) == 0) {
      const uint32_t count = (token >> 16) & 0xff;
      offset += count * ((token & (1u << 27)) ? 1 : 4);
    } else if (kind == 2 && (token & ~0x40ff000fu) == 0) {
      const uint32_t type = (token >> 16) & 0xff;
      const uint32_t bytes = TypeBytes(ctx, tokenAddress, type);
      if (type != 0x02) { shader.inputs[token & 15] = {int(stream), offset, type}; }
      offset += bytes;
    } else if (kind == 4 && (token & ~0x9e0000ffu) == 0) {
      const uint32_t count = (token >> 25) & 15;
      const uint32_t reg = token & 0xff;
      if (count && (reg >= 192 || count > 192 - reg)) {
        throw Fault(ctx, tokenAddress, 4, "guest vertex declaration constant register overflow");
      }
      std::array<uint32_t, 60> payload{};
      words.Read(payload.data(), count * 4);
      shader.decl.insert(shader.decl.end(), payload.begin(), payload.begin() + count * 4);
      for (uint32_t n = 0; n < count; ++n) {
        std::array<float, 4> value{};
        std::memcpy(value.data(), payload.data() + n * 4, sizeof(value));
        shader.constants.push_back({reg + n, value});
      }
    } else {
      throw Fault(ctx, tokenAddress, 4, "unsupported guest vertex declaration token");
    }
  }
  shader.stream_stride[stream] = std::max(shader.stream_stride[stream], offset);
}

uint32_t PackColor(const std::array<float, 4>& color) {
  const auto byte = [](float value) -> uint32_t {
    if (!(value > 0)) { return 0; }
    if (value >= 1) { return 255; }
    return uint32_t(value * 255.0f + 0.5f);
  };
  return byte(color[3]) << 24 | byte(color[0]) << 16 | byte(color[1]) << 8 | byte(color[2]);
}

}

gpu::VertexShader ReadVertexShader(Memory& memory, const X86Context& ctx, uint32_t handle, uint32_t declaration, uint32_t function) {
  if (!(handle & 1) || handle == 1) { throw Fault(ctx, handle, 0, "invalid guest vertex shader handle"); }
  auto lease = memory.Borrow();
  gpu::VertexShader shader;
  shader.handle = handle;
  if (declaration) { Declaration(memory, ctx, declaration, shader); }
  if (function) {
    ShaderWords words(memory, ctx, function, 1 + instructionLimit * 4);
    const uint32_t header = words.Word();
    const uint32_t type = header & 0xffff;
    const uint32_t count = header >> 16;
    if (type != 0x2078) { throw Fault(ctx, function, 4, "unsupported guest vertex program type"); }
    if (!count || count > instructionLimit) { throw Fault(ctx, function, 4, "invalid guest vertex program instruction count"); }
    shader.program_type = type;
    shader.program.resize(size_t(count) * 4);
    words.Read(shader.program.data(), count * 4);
  }
  return shader;
}

gpu::PixelShader ReadPixelShader(Memory& memory, const X86Context& ctx, uint32_t handle, uint32_t definition) {
  if (!handle || (handle & 1)) { throw Fault(ctx, handle, 0, "invalid guest pixel shader handle"); }
  if (!definition) { throw Fault(ctx, definition, 0xf0, "null guest pixel shader definition"); }
  gpu::PixelShader shader;
  shader.handle = handle;
  memory.Read(ctx, definition, shader.def, sizeof(shader.def));
  return shader;
}

void ApplyDiagnosticPixelConstants(gpu::State& state, uint32_t first, uint32_t count) {
  if (!state.ps) { return; }
  const auto* definition = state.ps->def;
  const auto selected = [&](uint32_t reg) { return reg < 8 && reg >= first && reg - first < count; };
  for (uint32_t n = 0; n < 8; ++n) {
    const uint32_t reg0 = (definition[57] >> (n * 4)) & 15;
    const uint32_t reg1 = (definition[58] >> (n * 4)) & 15;
    if (selected(reg0)) { state.simple[gpu::kMPsConstant0 / 4 + n] = PackColor(state.psc[reg0]); }
    if (selected(reg1)) { state.simple[gpu::kMPsConstant1 / 4 + n] = PackColor(state.psc[reg1]); }
  }
  const uint32_t final0 = definition[59] & 15;
  const uint32_t final1 = (definition[59] >> 4) & 15;
  if (selected(final0)) { state.simple[gpu::kMPsFinalConstant0 / 4] = PackColor(state.psc[final0]); }
  if (selected(final1)) { state.simple[gpu::kMPsFinalConstant1 / 4] = PackColor(state.psc[final1]); }
}

}
