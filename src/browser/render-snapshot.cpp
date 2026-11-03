#include "browser/render-snapshot.h"
#include <algorithm>
#include <bit>
#include <limits>

namespace browser {
namespace {

const auto require = [](bool valid, const char* reason) {
  if (!valid) { throw std::runtime_error(reason); }
};

struct Writer {
  std::vector<uint8_t> bytes;
  uint32_t limit;
  void Word(uint32_t& value) {
    require(bytes.size() <= limit && limit - bytes.size() >= 4, "render snapshot wire budget exceeded");
    for (uint32_t n = 0; n < 4; ++n) { bytes.push_back(uint8_t(value >> (n * 8))); }
  }
  void Blob(std::vector<uint8_t>& value, uint32_t maximum) {
    require(value.size() <= maximum, "render snapshot blob budget exceeded");
    uint32_t size = uint32_t(value.size());
    Word(size);
    require(bytes.size() <= limit && value.size() <= limit - bytes.size(), "render snapshot wire budget exceeded");
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
};

struct Reader {
  std::span<const uint8_t> bytes;
  size_t offset = 0;
  void Word(uint32_t& value) {
    require(offset <= bytes.size() && bytes.size() - offset >= 4, "truncated render snapshot word");
    value = 0;
    for (uint32_t n = 0; n < 4; ++n) { value |= uint32_t(bytes[offset++]) << (n * 8); }
  }
  void Blob(std::vector<uint8_t>& value, uint32_t maximum) {
    uint32_t size = 0;
    Word(size);
    require(size <= maximum && size <= bytes.size() - offset, "invalid render snapshot blob length");
    value.assign(bytes.begin() + offset, bytes.begin() + offset + size);
    offset += size;
  }
};

template <typename Codec> void Fields(Codec& codec, uint32_t& value) { codec.Word(value); }
template <typename Codec> void Fields(Codec& codec, uint64_t& value) {
  uint32_t low = uint32_t(value), high = uint32_t(value >> 32);
  codec.Word(low);
  codec.Word(high);
  value = uint64_t(low) | uint64_t(high) << 32;
}
template <typename Codec> void Fields(Codec& codec, int& value) {
  static_assert(sizeof(int) == 4);
  uint32_t bits = std::bit_cast<uint32_t>(value);
  codec.Word(bits);
  value = std::bit_cast<int>(bits);
}
template <typename Codec> void Fields(Codec& codec, float& value) {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
  uint32_t bits = std::bit_cast<uint32_t>(value);
  codec.Word(bits);
  value = std::bit_cast<float>(bits);
}
template <typename Codec, typename T, size_t Count> void Fields(Codec& codec, std::array<T, Count>& values) {
  for (auto& value : values) { Fields(codec, value); }
}
template <typename Codec, typename T, size_t Count> void Fields(Codec& codec, T (&values)[Count]) {
  for (auto& value : values) { Fields(codec, value); }
}
template <typename Codec, typename... Values> void Pack(Codec& codec, Values&... values) { (Fields(codec, values), ...); }
template <typename Codec> void Fields(Codec& codec, gpu::State::Stream& value) { Pack(codec, value.vb, value.stride); }
template <typename Codec> void Fields(Codec& codec, gpu::Surface& value) { Pack(codec, value.header, value.data, value.format, value.size); }
template <typename Codec> void Fields(Codec& codec, gpu::Viewport& value) { Pack(codec, value.x, value.y, value.w, value.h, value.min_z, value.max_z); }
template <typename Codec> void Fields(Codec& codec, gpu::VertexShader::Input& value) { Pack(codec, value.stream, value.offset, value.type); }
template <typename Codec> void Fields(Codec& codec, std::pair<uint32_t, std::array<float, 4>>& value) { Pack(codec, value.first, value.second); }

template <typename Codec, typename T> void Vector(Codec& codec, std::vector<T>& values, uint32_t maximum) {
  require(values.size() <= maximum, "render snapshot vector budget exceeded");
  uint32_t size = uint32_t(values.size());
  codec.Word(size);
  require(size <= maximum, "render snapshot vector budget exceeded");
  if constexpr (std::is_same_v<Codec, Reader>) {
    require(size <= (codec.bytes.size() - codec.offset) / 4, "truncated render snapshot vector");
    values.resize(size);
  }
  for (auto& value : values) { Fields(codec, value); }
}

template <typename Codec> void StateFields(Codec& codec, gpu::State& state) {
  Pack(codec, state.simple, state.rs, state.tss, state.textures, state.streams,
    state.index_buffer, state.index_data, state.base_vertex, state.vs_handle, state.vsc, state.psc,
    state.viewport, state.rt, state.zs, state.scissor_count, state.scissors, state.transforms,
    state.material, state.lights, state.light_enable, state.ss_offset_x, state.ss_offset_y, state.zscale);
}

template <typename Codec> void SnapshotFields(Codec& codec, RenderSnapshot& snapshot, const RenderSnapshotLimits& limits) {
  uint32_t magic = 0x504e5352, version = 1;
  Pack(codec, magic, version);
  require(magic == 0x504e5352 && version == 1, "unsupported render snapshot wire format");
  StateFields(codec, snapshot.state);
  uint32_t hasVertex = snapshot.vertexShader.has_value(), hasPixel = snapshot.pixelShader.has_value();
  Pack(codec, hasVertex, hasPixel);
  require(hasVertex <= 1 && hasPixel <= 1, "invalid render snapshot shader presence");
  if (hasVertex) {
    if (!snapshot.vertexShader) { snapshot.vertexShader.emplace(); }
    auto& shader = *snapshot.vertexShader;
    Pack(codec, shader.handle, shader.program_type, shader.hash, shader.inputs, shader.stream_stride);
    Vector(codec, shader.decl, limits.maxShaderWords);
    Vector(codec, shader.program, limits.maxShaderWords);
    Vector(codec, shader.constants, limits.maxShaderConstants);
  }
  if (hasPixel) {
    if (!snapshot.pixelShader) { snapshot.pixelShader.emplace(); }
    Pack(codec, snapshot.pixelShader->handle, snapshot.pixelShader->def);
  }
  uint32_t count = uint32_t(snapshot.resources.size());
  codec.Word(count);
  require(count <= limits.maxResources, "render snapshot resource budget exceeded");
  if constexpr (std::is_same_v<Codec, Reader>) {
    require(count <= (codec.bytes.size() - codec.offset) / 36, "truncated render snapshot resources");
    snapshot.resources.resize(count);
  }
  for (auto& resource : snapshot.resources) {
    uint32_t kind = uint32_t(resource.kind);
    Pack(codec, resource.id, resource.version, kind, resource.headerAddress, resource.payloadAddress);
    resource.kind = RenderResourceKind(kind);
    codec.Blob(resource.header, 20);
    codec.Blob(resource.payload, limits.maxPayloadBytes);
  }
}

uint32_t HeaderWord(const RenderResourceSnapshot& resource, size_t offset) {
  require(offset + 4 <= resource.header.size(), "short render resource header");
  uint32_t value = 0;
  for (uint32_t n = 0; n < 4; ++n) { value |= uint32_t(resource.header[offset + n]) << (n * 8); }
  return value;
}

uint32_t PhysicalAddress(uint32_t address) {
  if ((address >= 0x80000000 && address < 0x84000000) || (address >= 0xf0000000 && address < 0xf4000000)) {
    return address & 0x03ffffff;
  }
  return address;
}

void ValidateShader(const gpu::VertexShader& shader, const RenderSnapshotLimits& limits) {
  require(shader.decl.size() <= limits.maxShaderWords && shader.program.size() <= limits.maxShaderWords &&
    shader.constants.size() <= limits.maxShaderConstants, "render snapshot shader budget exceeded");
  require(shader.program.size() % 4 == 0, "invalid vertex program word count");
  for (const auto& input : shader.inputs) { require(input.stream >= -1 && input.stream < 16, "invalid vertex shader stream"); }
}

void Validate(const RenderSnapshot& snapshot, const RenderSnapshotLimits& limits) {
  require(!snapshot.state.vs && !snapshot.state.ps, "render snapshot cannot contain shared shaders");
  require(snapshot.state.scissor_count <= 8, "render snapshot scissor count exceeds capacity");
  require(snapshot.resources.size() <= limits.maxResources, "render snapshot resource budget exceeded");
  if (snapshot.vertexShader) { ValidateShader(*snapshot.vertexShader, limits); }
  uint64_t bytes = 0;
  for (const auto& resource : snapshot.resources) {
    require(resource.id && resource.version, "render resource identity and version must be nonzero");
    require(resource.payload.size() && resource.payload.size() <= limits.maxPayloadBytes, "invalid render resource payload size");
    require(resource.payloadAddress && uint64_t(resource.payloadAddress) + resource.payload.size() <= (uint64_t(1) << 32), "invalid render resource payload range");
    const auto kind = resource.kind;
    require(kind >= RenderResourceKind::VertexBuffer && kind <= RenderResourceKind::IndexData, "invalid render resource kind");
    if (kind == RenderResourceKind::IndexData) {
      require(!resource.headerAddress && resource.header.empty(), "raw index data cannot have a header");
    } else {
      const bool shortHeader = kind == RenderResourceKind::VertexBuffer || kind == RenderResourceKind::IndexBuffer;
      require(resource.headerAddress && resource.header.size() == (shortHeader ? 12 : 20), "invalid render resource header size");
      require(uint64_t(resource.headerAddress) + resource.header.size() <= (uint64_t(1) << 32), "invalid render resource header range");
      const uint32_t type = HeaderWord(resource, 0) & 0x70000;
      const uint32_t expected = kind == RenderResourceKind::VertexBuffer ? 0 : kind == RenderResourceKind::IndexBuffer ? 0x10000 : kind == RenderResourceKind::Texture ? 0x40000 : 0x50000;
      require(type == expected, "render resource header type mismatch");
      const uint32_t data = HeaderWord(resource, 4);
      require(PhysicalAddress(data) == PhysicalAddress(resource.payloadAddress), "render resource payload does not match header data");
    }
    bytes += resource.header.size() + resource.payload.size();
    require(bytes <= limits.maxTotalBytes, "render snapshot total resource budget exceeded");
    for (const auto& other : snapshot.resources) {
      if (&other == &resource) { break; }
      require(!resource.headerAddress || other.headerAddress != resource.headerAddress, "duplicate render resource header");
      if (other.id == resource.id) {
        const auto storageView = [](RenderResourceKind value) {
          return value == RenderResourceKind::Texture || value == RenderResourceKind::Surface;
        };
        require(storageView(other.kind) && storageView(resource.kind) && other.version == resource.version &&
          PhysicalAddress(other.payloadAddress) == PhysicalAddress(resource.payloadAddress) && other.payload == resource.payload &&
          (HeaderWord(other, 12) & ~8u) == (HeaderWord(resource, 12) & ~8u) && HeaderWord(other, 16) == HeaderWord(resource, 16),
          "conflicting render resource storage identity");
      }
    }
  }
  const auto bound = [&](uint32_t address, RenderResourceKind kind) {
    if (!address) { return; }
    require(std::any_of(snapshot.resources.begin(), snapshot.resources.end(), [&](const auto& resource) {
      return resource.headerAddress == address && resource.kind == kind;
    }), "bound render resource missing from snapshot");
  };
  for (const auto address : snapshot.state.textures) { bound(address, RenderResourceKind::Texture); }
  for (const auto& stream : snapshot.state.streams) { bound(stream.vb, RenderResourceKind::VertexBuffer); }
  bound(snapshot.state.index_buffer, RenderResourceKind::IndexBuffer);
  bound(snapshot.state.rt.header, RenderResourceKind::Surface);
  bound(snapshot.state.zs.header, RenderResourceKind::Surface);
  if (snapshot.state.index_data) {
    require(std::any_of(snapshot.resources.begin(), snapshot.resources.end(), [&](const auto& resource) {
      return (resource.kind == RenderResourceKind::IndexBuffer || resource.kind == RenderResourceKind::IndexData) &&
        PhysicalAddress(snapshot.state.index_data) >= PhysicalAddress(resource.payloadAddress) &&
        uint64_t(PhysicalAddress(snapshot.state.index_data)) < uint64_t(PhysicalAddress(resource.payloadAddress)) + resource.payload.size();
    }), "bound index data missing from snapshot");
  }
}

}

RenderSnapshot CaptureRenderSnapshot(const X86Context& context, Memory& memory, const gpu::State& state,
  std::span<const RenderResourceRequest> resources, const RenderSnapshotLimits& limits) {
  require(resources.size() <= limits.maxResources, "render snapshot resource budget exceeded");
  require(state.scissor_count <= 8, "render snapshot scissor count exceeds capacity");
  if (state.vs) { ValidateShader(*state.vs, limits); }
  RenderSnapshot snapshot;
  snapshot.state = state;
  snapshot.state.vs.reset();
  snapshot.state.ps.reset();
  if (state.vs) { snapshot.vertexShader = *state.vs; }
  if (state.ps) { snapshot.pixelShader = *state.ps; }
  auto lease = memory.Borrow();
  uint64_t total = 0;
  for (const auto& request : resources) {
    require(request.id && request.version, "render resource identity and version must be nonzero");
    require(request.payloadBytes && request.payloadBytes <= limits.maxPayloadBytes, "render resource payload budget exceeded");
    require(request.payloadAddress && uint64_t(request.payloadAddress) + request.payloadBytes <= (uint64_t(1) << 32), "invalid render resource payload range");
    RenderResourceSnapshot resource;
    resource.id = request.id;
    resource.version = request.version;
    resource.kind = request.kind;
    resource.headerAddress = request.headerAddress;
    resource.payloadAddress = request.payloadAddress;
    const uint32_t headerBytes = request.kind == RenderResourceKind::IndexData ? 0 : request.kind == RenderResourceKind::VertexBuffer || request.kind == RenderResourceKind::IndexBuffer ? 12 : 20;
    total += uint64_t(headerBytes) + request.payloadBytes;
    require(total <= limits.maxTotalBytes, "render snapshot total resource budget exceeded");
    if (headerBytes) {
      require(request.headerAddress && uint64_t(request.headerAddress) + headerBytes <= (uint64_t(1) << 32), "invalid render resource header range");
      resource.header.resize(headerBytes);
      memory.Read(context, request.headerAddress, resource.header.data(), resource.header.size());
    }
    memory.CheckRange(context, request.payloadAddress, request.payloadBytes);
    resource.payload.resize(request.payloadBytes);
    memory.Read(context, request.payloadAddress, resource.payload.data(), resource.payload.size());
    snapshot.resources.push_back(std::move(resource));
  }
  Validate(snapshot, limits);
  SerializeRenderSnapshot(snapshot, limits);
  return snapshot;
}

std::vector<uint8_t> SerializeRenderSnapshot(const RenderSnapshot& snapshot, const RenderSnapshotLimits& limits) {
  Validate(snapshot, limits);
  auto owned = snapshot;
  Writer writer{{}, limits.maxTotalBytes};
  SnapshotFields(writer, owned, limits);
  return std::move(writer.bytes);
}

RenderSnapshot DeserializeRenderSnapshot(std::span<const uint8_t> bytes, const RenderSnapshotLimits& limits) {
  require(bytes.size() <= limits.maxTotalBytes, "render snapshot wire budget exceeded");
  RenderSnapshot snapshot;
  Reader reader{bytes};
  SnapshotFields(reader, snapshot, limits);
  require(reader.offset == bytes.size(), "trailing render snapshot bytes");
  Validate(snapshot, limits);
  return snapshot;
}

}
