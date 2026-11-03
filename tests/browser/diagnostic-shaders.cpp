#include "browser/diagnostic-shaders.h"
#include <bit>
#include <cstdio>
#include <limits>

const auto check = [](bool valid, const char* reason) {
  if (!valid) { throw std::runtime_error(reason); }
};

int main() {
  try {
    browser::Memory memory;
    X86Context ctx{};
    ctx.last_icall = 0x12345678;
    ctx.eax = 0xabcdef01;
    memory.Ensure(0x10000, 0x10000);
    const auto write = [&](uint32_t address, const std::vector<uint32_t>& words) {
      memory.Write(ctx, address, words.data(), words.size() * 4);
    };
    const auto reject = [&](auto operation, const char* reason) {
      bool rejected = false;
      try { operation(); } catch (const browser::Fault& fault) {
        rejected = true;
        check(fault.context && fault.context->last_icall == ctx.last_icall && fault.context->eax == ctx.eax, "fault preserves guest context");
      }
      check(rejected, reason);
    };
    const auto vertex = [&](uint32_t declaration = 0x10000, uint32_t function = 0x12000) {
      return browser::ReadVertexShader(memory, ctx, 0x18001, declaration, function);
    };
    const std::vector<uint32_t> declaration{
      0, 0x20000002, 0x40320000, 0x50020000, 0x40220009,
      0x20000003, 0x58030000, 0x40720007, 0x20000002, 0x40400003, 0x40020004,
      0x840000be, 0x3f800000, 0xc0000000, 0xffffffff, 0x80000000,
      0x40800000, 0x40a00000, 0x40c00000, 0x40e00000, 0xffffffff
    };
    write(0x10000, declaration);
    write(0x12000, {0x00012078, 0, 0x00200000, 0, 1});
    const auto before = ctx;
    const auto shader = vertex();
    check(shader.handle == 0x18001 && shader.decl == declaration && shader.program_type == 0x2078 && shader.program == std::vector<uint32_t>{0, 0x00200000, 0, 1}, "owned vertex shader preserves guest words and identity");
    check(shader.inputs[0].stream == 2 && shader.inputs[0].offset == 0 && shader.inputs[0].type == 0x32, "stream input decoded");
    check(shader.inputs[9].offset == 20 && shader.inputs[3].offset == 28 && shader.stream_stride[2] == 32, "skip and repeated stream resume offsets");
    check(shader.inputs[7].stream == 3 && shader.inputs[7].offset == 3 && shader.stream_stride[3] == 15, "FLOAT2H consumes three floats after byte skip");
    check(shader.inputs[4].stream == -1 && shader.inputs[15].stream == -1, "NONE and undeclared registers consume no input");
    check(shader.constants.size() == 2 && shader.constants[0].first == 190 && shader.constants[1].first == 191, "declared constants retain hardware register indices");
    check(shader.constants[0].second[0] == 1 && shader.constants[0].second[1] == -2 && std::bit_cast<uint32_t>(shader.constants[0].second[2]) == 0xffffffff && std::bit_cast<uint32_t>(shader.constants[0].second[3]) == 0x80000000, "constant payload bits including END value are not tokens");
    check(shader.constants[1].second == std::array<float, 4>{4, 5, 6, 7}, "multiple constants decoded");
    check(!std::memcmp(&ctx, &before, sizeof(ctx)), "successful helper preserves guest context");
    write(0x10000, {0xffffffff});
    write(0x12000, {0x00012078, 9, 8, 7, 6});
    check(shader.decl == declaration && shader.program[0] == 0, "guest mutation cannot modify owned result");
    check(vertex(0, 0).decl.empty() && vertex(0, 0).program.empty(), "null optional declaration and function accepted");
    check(vertex(0x10000, 0).program_type == 0, "declaration only shader supported");
    reject([&] { browser::ReadVertexShader(memory, ctx, 4, 0, 0); }, "FVF handle is not a shader record");
    reject([&] { browser::ReadVertexShader(memory, ctx, 1, 0, 0); }, "null vertex record rejected");
    for (const uint32_t header : {0x00017378u, 0x00017778u, 0x00011234u, 0x00002078u, 0x00892078u, 0xffff2078u}) {
      write(0x12000, {header});
      reject([&] { vertex(); }, "unsupported or invalid vertex program header rejected");
    }
    std::vector<uint32_t> maximumProgram(1 + 136 * 4, 0);
    maximumProgram[0] = 0x00882078;
    maximumProgram.back() = 1;
    write(0x12000, maximumProgram);
    check(vertex().program.size() == 136 * 4, "hardware maximum vertex instruction count accepted");
    for (const uint32_t token : {1u, 0x20000010u, 0x30000000u, 0x60000000u, 0xa0000000u, 0xc0000000u, 0xe0000000u, 0x40ff0000u, 0x40420010u, 0x50000001u, 0x80000100u, 0x840000bfu}) {
      write(0x10000, {token, 0xffffffff});
      reject([&] { vertex(0x10000, 0); }, "unsupported malformed or out of range declaration rejected");
    }
    const std::array<std::pair<uint32_t, uint32_t>, 20> types{{
      {0x02, 0}, {0x12, 4}, {0x22, 8}, {0x32, 12}, {0x42, 16}, {0x72, 12}, {0x40, 4},
      {0x11, 2}, {0x21, 4}, {0x31, 6}, {0x41, 8}, {0x14, 1}, {0x24, 2}, {0x34, 3}, {0x44, 4},
      {0x15, 2}, {0x25, 4}, {0x35, 6}, {0x45, 8}, {0x16, 4}
    }};
    for (const auto& [type, bytes] : types) {
      write(0x10000, {0x40000000 | (type << 16), 0xffffffff});
      check(vertex(0x10000, 0).stream_stride[0] == bytes, "supported declaration type byte width");
    }
    std::vector<uint32_t> maximumDeclaration(1024, 0);
    maximumDeclaration.back() = 0xffffffff;
    write(0x10000, maximumDeclaration);
    check(vertex(0x10000, 0).decl.size() == 1024, "declaration terminator at limit accepted");
    maximumDeclaration.back() = 0;
    maximumDeclaration.push_back(0xffffffff);
    write(0x10000, maximumDeclaration);
    reject([&] { vertex(0x10000, 0); }, "declaration must terminate inside token budget");
    maximumDeclaration.resize(1024);
    maximumDeclaration.back() = 0x82000000;
    write(0x10000, maximumDeclaration);
    reject([&] { vertex(0x10000, 0); }, "constant payload counts toward declaration token budget");
    write(0x1fffc, {0x82000000});
    reject([&] { vertex(0x1fffc, 0); }, "truncated declared constant faults");
    write(0x1fffc, {0x00012078});
    reject([&] { vertex(0, 0x1fffc); }, "truncated program faults");
    reject([&] { vertex(0x30000, 0); }, "unmapped declaration faults");
    reject([&] { vertex(0, 0x30000); }, "unmapped program faults");
    memory.Ensure(0xfffff000, 0x1000);
    write(0xfffffffc, {0xffffffff});
    check(vertex(0xfffffffc, 0).decl == std::vector<uint32_t>{0xffffffff}, "declaration may end at final guest word");
    write(0xffffffec, {0x00012078, 0, 0, 0, 1});
    check(vertex(0, 0xffffffec).program.size() == 4, "program may end at guest address space boundary");
    write(0xfffffffc, {0});
    reject([&] { vertex(0xfffffffc, 0); }, "declaration cursor cannot wrap guest address space");
    write(0xfffffffc, {0x00012078});
    reject([&] { vertex(0, 0xfffffffc); }, "program payload cannot wrap guest address space");
    std::vector<uint32_t> definition(60);
    for (uint32_t n = 0; n < definition.size(); ++n) { definition[n] = 0x12340000 + n; }
    write(0x16000, definition);
    const auto pixel = browser::ReadPixelShader(memory, ctx, 0x18000, 0x16000);
    check(pixel.handle == 0x18000, "pixel identity preserved");
    for (uint32_t n = 0; n < definition.size(); ++n) { check(pixel.def[n] == definition[n], "complete pixel definition copied"); }
    write(0x16000, std::vector<uint32_t>(60, 0));
    check(pixel.def[59] == definition[59], "pixel result owns guest definition");
    reject([&] { browser::ReadPixelShader(memory, ctx, 0, 0x16000); }, "null pixel record rejected");
    reject([&] { browser::ReadPixelShader(memory, ctx, 0x18001, 0x16000); }, "tagged pixel record rejected");
    reject([&] { browser::ReadPixelShader(memory, ctx, 0x18000, 0); }, "null pixel definition rejected");
    reject([&] { browser::ReadPixelShader(memory, ctx, 0x18000, 0x1ff14); }, "pixel definition requires all sixty words");
    reject([&] { browser::ReadPixelShader(memory, ctx, 0x18000, 0xffffff80); }, "pixel definition wraparound rejected");
    memory.Ensure(0x30000, 0x1000);
    memory.Ensure(0x31000, 0x1000);
    write(0x30ffc, {0x820000bf, 0x3f800000, 0x40000000, 0x40400000, 0x40800000, 0xffffffff});
    check(vertex(0x30ffc, 0).constants[0].second == std::array<float, 4>{1, 2, 3, 4}, "checked declaration read crosses adjacent backing regions");
    write(0x30ff8, definition);
    check(browser::ReadPixelShader(memory, ctx, 0x18000, 0x30ff8).def[59] == definition[59], "checked pixel read crosses adjacent backing regions");
    write(0x30ffc, {0x00012078, 0, 0, 0, 1});
    check(vertex(0, 0x30ffc).program.size() == 4, "checked program read crosses adjacent backing regions");
    memory.Decommit(0x31000, 0x1000);
    reject([&] { browser::ReadPixelShader(memory, ctx, 0x18000, 0x30ff8); }, "uncommitted pixel tail rejected");
    reject([&] { vertex(0, 0x30ffc); }, "uncommitted vertex program payload rejected");
    write(0x10000, {0x800000ff, 0xffffffff});
    check(vertex(0x10000, 0).constants.empty(), "zero constant count consumes no payload");
    gpu::State state;
    browser::ApplyDiagnosticPixelConstants(state, 0, 8);
    state.ps = std::make_shared<gpu::PixelShader>(pixel);
    state.ps->def[57] = 0xfffffff0;
    state.ps->def[58] = 0xffffff1f;
    state.ps->def[59] = 0x10;
    state.psc[0] = {1, 0.5f, -1, 0.25f};
    state.psc[1] = {0, 1, 0.25f, 1};
    state.simple[gpu::kMPsConstant0 / 4 + 1] = 0x12345678;
    state.simple[gpu::kMPsConstant1 / 4] = 0xabcdef01;
    browser::ApplyDiagnosticPixelConstants(state, 0, 1);
    check(state.simple[gpu::kMPsConstant0 / 4] == 0x40ff8000 && state.simple[gpu::kMPsFinalConstant0 / 4] == 0x40ff8000, "pixel mappings pack RGBA to rounded ARGB");
    check(state.simple[gpu::kMPsConstant0 / 4 + 1] == 0x12345678 && state.simple[gpu::kMPsConstant1 / 4] == 0xabcdef01 && state.simple[gpu::kMPsFinalConstant1 / 4] == 0, "literal and unselected pixel mappings preserved");
    browser::ApplyDiagnosticPixelConstants(state, 1, 1);
    check(state.simple[gpu::kMPsConstant1 / 4 + 1] == 0xff00ff40 && state.simple[gpu::kMPsFinalConstant1 / 4] == 0xff00ff40, "C1 and final pixel mapping selected");
    state.psc[0] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 2};
    browser::ApplyDiagnosticPixelConstants(state, 0, 1);
    check(state.simple[gpu::kMPsConstant0 / 4] == 0xff00ff00, "nonfinite guest pixel constants pack without undefined conversion");
    state.psc[0] = {1, 1, 1, 1};
    browser::ApplyDiagnosticPixelConstants(state, UINT32_MAX, 2);
    check(state.simple[gpu::kMPsConstant0 / 4] == 0xff00ff00, "pixel selection cannot wrap register range");
    browser::ApplyDiagnosticPixelConstants(state, 0, 0);
    check(state.simple[gpu::kMPsConstant0 / 4] == 0xff00ff00, "zero pixel range changes no constants");
    std::puts("diagnostic-shaders-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
