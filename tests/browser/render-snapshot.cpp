#include "browser/render-snapshot.h"
#include <bit>
#include <cstdio>
#include <stdexcept>

const auto check = [](bool valid, const char* name) {
  if (!valid) { throw std::runtime_error(name); }
};
const auto rejects = [](auto operation, const char* name) {
  bool rejected = false;
  try { operation(); } catch (const std::exception&) { rejected = true; }
  check(rejected, name);
};

int main() {
  try {
    browser::Memory memory;
    X86Context context{};
    memory.Map(0x10000, 8192);
    memory.Commit(0x10000, 8192);
    const uint32_t header[] = {0x00040001, 0x11000, 0, 0x12345678, 0xabcdef01};
    memory.Write(context, 0x10000, header, sizeof(header));
    const uint8_t pixels[] = {1, 2, 3, 4};
    memory.Write(context, 0x11000, pixels, sizeof(pixels));
    gpu::State state;
    state.textures[0] = state.textures[2] = 0x10000;
    state.simple[2047] = 0xabcdef12;
    state.rs[165] = 0xdead1234;
    state.tss[3][31] = 0x88776655;
    state.vsc[191] = {1, 2, 3, 4};
    state.psc[15] = {5, 6, 7, 8};
    state.viewport = {11, 12, 300, 400, 0.25f, 0.75f};
    state.material[16] = 4.25f;
    state.lights[7][25] = 0x12345678;
    state.light_enable = 128;
    state.base_vertex = 42;
    state.ss_offset_x = 0.25f;
    state.ss_offset_y = -0.25f;
    state.zscale = 65535;
    state.transforms[511][15] = std::bit_cast<float>(0x7fc01234u);
    state.scissor_count = 1;
    state.scissors[3] = 42;
    state.streams[15].stride = 128;
    state.vs_handle = 3;
    state.vs = std::make_shared<gpu::VertexShader>();
    state.vs->handle = 3;
    state.vs->decl = {1, 2, 0xffffffff};
    state.vs->program = {4, 5, 6, 7};
    state.vs->inputs[15] = {2, 16, 3};
    state.vs->constants.push_back({7, {1, 2, 3, 4}});
    state.ps = std::make_shared<gpu::PixelShader>();
    state.ps->def[59] = 123;
    const browser::RenderResourceRequest request{17, 9, browser::RenderResourceKind::Texture, 0x10000, 0x11000, 4};
    auto snapshot = browser::CaptureRenderSnapshot(context, memory, state, {&request, 1});
    check(!snapshot.state.vs && !snapshot.state.ps, "state contains no shared shader aliases");
    state.vs->decl[0] = 99;
    state.ps->def[59] = 0;
    memory.Decommit(0x10000, 8192);
    check(snapshot.vertexShader->decl[0] == 1 && snapshot.pixelShader->def[59] == 123, "shader data owned");
    check(snapshot.resources[0].payload[3] == 4 && snapshot.resources[0].header.size() == 20, "guest bytes owned");
    auto bytes = browser::SerializeRenderSnapshot(snapshot);
    check(bytes[0] == 'R' && bytes[1] == 'S' && bytes[4] == 1, "portable versioned wire header");
    auto decoded = browser::DeserializeRenderSnapshot(bytes);
    check(browser::SerializeRenderSnapshot(decoded) == bytes, "canonical full state roundtrip");
    check(decoded.state.simple == snapshot.state.simple && decoded.state.rs == snapshot.state.rs && decoded.state.tss == snapshot.state.tss, "render and texture state words preserved");
    check(decoded.state.vsc == snapshot.state.vsc && decoded.state.psc == snapshot.state.psc, "shader constant registers preserved");
    check(decoded.state.material == snapshot.state.material && decoded.state.lights == snapshot.state.lights, "fixed function material and lights preserved");
    check(decoded.state.viewport.x == 11 && decoded.state.viewport.y == 12 && decoded.state.viewport.w == 300 && decoded.state.viewport.h == 400 && decoded.state.viewport.min_z == 0.25f && decoded.state.viewport.max_z == 0.75f, "viewport fields preserved");
    check(decoded.state.streams[15].stride == 128 && decoded.state.base_vertex == 42 && decoded.state.scissor_count == 1 && decoded.state.scissors[3] == 42, "draw and scissor fields preserved");
    check(decoded.state.light_enable == 128 && decoded.state.ss_offset_x == 0.25f && decoded.state.ss_offset_y == -0.25f && decoded.state.zscale == 65535, "remaining scalar fields preserved");
    uint64_t checksum = 1469598103934665603ull;
    for (const auto byte : bytes) { checksum = (checksum ^ byte) * 1099511628211ull; }
    std::printf("wire-%zu-%016llx\n", bytes.size(), static_cast<unsigned long long>(checksum));
    check(std::bit_cast<uint32_t>(decoded.state.transforms[511][15]) == 0x7fc01234u, "float bits preserved");
    check(decoded.resources[0].id == 17 && decoded.resources[0].version == 9, "explicit identity and version");
    check(decoded.vertexShader->inputs[15].stream == 2 && decoded.vertexShader->constants[0].first == 7, "shader metadata preserved");
    auto copied = decoded;
    copied.vertexShader->decl[0] = 22;
    check(decoded.vertexShader->decl[0] == 1, "snapshot copies do not alias shaders");
    rejects([&] { browser::CaptureRenderSnapshot(context, memory, state, {}); }, "missing bound resource rejected");
    rejects([&] { browser::CaptureRenderSnapshot(context, memory, state, {&request, 1}); }, "decommitted resource rejected");
    memory.Commit(0x10000, 8192);
    memory.Write(context, 0x10000, header, sizeof(header));
    memory.Write(context, 0x11000, pixels, sizeof(pixels));
    auto invalid = request;
    invalid.id = 0;
    rejects([&] { browser::CaptureRenderSnapshot(context, memory, state, {&invalid, 1}); }, "zero identity rejected");
    invalid = request;
    invalid.payloadAddress = 0x11004;
    rejects([&] { browser::CaptureRenderSnapshot(context, memory, state, {&invalid, 1}); }, "unrelated payload rejected");
    const browser::RenderResourceRequest duplicates[] = {request, request};
    rejects([&] { browser::CaptureRenderSnapshot(context, memory, state, duplicates); }, "duplicate resource identity rejected");
    invalid = request;
    invalid.payloadAddress = 0xfffffff0;
    invalid.payloadBytes = 32;
    rejects([&] { browser::CaptureRenderSnapshot(context, memory, state, {&invalid, 1}); }, "wrapping payload rejected");
    browser::RenderSnapshotLimits limits;
    limits.maxPayloadBytes = 3;
    rejects([&] { browser::CaptureRenderSnapshot(context, memory, state, {&request, 1}, limits); }, "payload cap enforced");
    limits = {};
    limits.maxShaderWords = 2;
    rejects([&] { browser::CaptureRenderSnapshot(context, memory, state, {&request, 1}, limits); }, "shader cap enforced before copying");
    state.scissor_count = 9;
    rejects([&] { browser::CaptureRenderSnapshot(context, memory, state, {&request, 1}); }, "scissor count bounded");
    state.scissor_count = 1;
    auto corrupted = bytes;
    corrupted[4] = 2;
    rejects([&] { browser::DeserializeRenderSnapshot(corrupted); }, "unknown version rejected");
    for (size_t n = 0; n < bytes.size(); n += 257) {
      rejects([&] { browser::DeserializeRenderSnapshot({bytes.data(), n}); }, "truncated wire rejected");
    }
    auto malformedLength = bytes;
    for (size_t n = malformedLength.size() - 8; n < malformedLength.size() - 4; ++n) { malformedLength[n] = 0xff; }
    rejects([&] { browser::DeserializeRenderSnapshot(malformedLength); }, "oversized wire blob rejected before allocation");
    limits = {};
    limits.maxTotalBytes = 1024;
    rejects([&] { browser::SerializeRenderSnapshot(snapshot, limits); }, "full wire budget includes scalar state");
    bytes.push_back(0);
    rejects([&] { browser::DeserializeRenderSnapshot(bytes); }, "trailing bytes rejected");
    gpu::State bufferState;
    bufferState.streams[0] = {0x10ff4, 4};
    const uint32_t bufferHeader[] = {1, 0x11000, 0};
    memory.Write(context, 0x10ff4, bufferHeader, sizeof(bufferHeader));
    const browser::RenderResourceRequest buffer{18, 1, browser::RenderResourceKind::VertexBuffer, 0x10ff4, 0x11000, 4};
    check(browser::CaptureRenderSnapshot(context, memory, bufferState, {&buffer, 1}).resources[0].header.size() == 12, "short buffer header not overread");
    memory.Write(context, 0x10040, header, sizeof(header));
    gpu::State aliasState;
    aliasState.textures[0] = 0x10000;
    aliasState.textures[1] = 0x10040;
    const browser::RenderResourceRequest aliases[] = {request, {request.id, request.version, request.kind, 0x10040, request.payloadAddress, request.payloadBytes}};
    auto aliasSnapshot = browser::CaptureRenderSnapshot(context, memory, aliasState, aliases);
    check(aliasSnapshot.resources[0].id == aliasSnapshot.resources[1].id, "compatible distinct views retain physical storage identity");
    check(browser::DeserializeRenderSnapshot(browser::SerializeRenderSnapshot(aliasSnapshot)).resources.size() == 2, "shared physical storage views roundtrip without wire schema change");
    auto borderAlias = aliasSnapshot;
    borderAlias.resources[1].header[12] ^= 8;
    check(browser::DeserializeRenderSnapshot(browser::SerializeRenderSnapshot(borderAlias)).resources.size() == 2,
      "Xbox format border-source selector does not change shared physical storage layout");
    auto inconsistentBorder = borderAlias;
    inconsistentBorder.resources[1].header[12] ^= 4;
    rejects([&] { browser::SerializeRenderSnapshot(inconsistentBorder); }, "border-source normalization never erases cube storage differences");
    inconsistentBorder = borderAlias;
    inconsistentBorder.resources[1].header[13] ^= 1;
    rejects([&] { browser::SerializeRenderSnapshot(inconsistentBorder); }, "border-source normalization never erases pixel format differences");
    inconsistentBorder = borderAlias;
    inconsistentBorder.resources[1].header[16] ^= 1;
    rejects([&] { browser::SerializeRenderSnapshot(inconsistentBorder); }, "border-source normalization never erases extent or pitch differences");
    auto inconsistentAlias = aliasSnapshot;
    ++inconsistentAlias.resources[1].version;
    rejects([&] { browser::SerializeRenderSnapshot(inconsistentAlias); }, "same storage identity cannot have conflicting versions");
    inconsistentAlias = aliasSnapshot;
    inconsistentAlias.resources[1].payload[0] ^= 1;
    rejects([&] { browser::SerializeRenderSnapshot(inconsistentAlias); }, "same storage identity cannot have conflicting bytes");
    inconsistentAlias = aliasSnapshot;
    inconsistentAlias.resources[1].header[12] ^= 1;
    rejects([&] { browser::SerializeRenderSnapshot(inconsistentAlias); }, "same storage identity cannot have conflicting view formats");
    inconsistentAlias = aliasSnapshot;
    inconsistentAlias.resources[1] = inconsistentAlias.resources[0];
    rejects([&] { browser::SerializeRenderSnapshot(inconsistentAlias); }, "same header cannot be duplicated even for compatible storage");
    std::puts("render-snapshot-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
