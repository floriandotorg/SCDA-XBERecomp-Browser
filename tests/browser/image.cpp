#include "browser/image.h"
#include <array>
#include <cassert>
#include <cstdio>

namespace {
const auto put = [](std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value) {
  for (uint32_t n = 0; n < 4; ++n) { bytes.at(offset + n) = uint8_t(value >> (n * 8)); }
};
const auto fixture = [] {
  std::vector<uint8_t> bytes(0x800);
  std::memcpy(bytes.data(), "XBEH", 4);
  for (const auto& [offset, value] : std::vector<std::pair<uint32_t, uint32_t>>{
    {0x104, 0x10000}, {0x108, 0x400}, {0x10c, 0x3001}, {0x118, 0x10200},
    {0x11c, 3}, {0x120, 0x10240}, {0x128, 0x11000 ^ xbe::kRetailEntryKey},
    {0x12c, 0x12000}, {0x158, 0x12040 ^ xbe::kRetailThunkKey}, {0x200, 12},
    {0x240, xbe::kExecutable | xbe::kPreload}, {0x244, 0x11000}, {0x248, 0x200},
    {0x24c, 0x400}, {0x250, 0x200}, {0x254, 0x10300},
    {0x278, xbe::kWritable | xbe::kPreload}, {0x27c, 0x12000}, {0x280, 0x200},
    {0x284, 0x600}, {0x288, 0x100}, {0x28c, 0x10300},
    {0x2b0, xbe::kWritable}, {0x2b4, 0x12200}, {0x2b8, 0x200},
    {0x2bc, 0x700}, {0x2c0, 0x100}, {0x2c4, 0x10300},
    {0x600, 0x12080}, {0x604, 0x12084}, {0x608, 0x12100}, {0x60c, 0x12050},
    {0x610, 5}, {0x640, 0x80000001}, {0x644, 0x8000012e}, {0x650, 0x11010},
    {0x680, 0x44332211}, {0x400, 0x12345678}, {0x700, 0xabcdef01},
  }) { put(bytes, offset, value); }
  return bytes;
};
const browser::ImageLayout layout{0x10000, 0x13001, 0x11000};
const auto scalar = [](browser::PreparedImage& prepared, uint32_t address) {
  uint32_t value = 0;
  prepared.memory->Read({}, address, &value, 4);
  return value;
};
const auto service = [](X86Context& context, uint8_t*) { context.eax = 0x76543210; };
const auto tlsService = [](X86Context& context, uint8_t* base) {
  auto& memory = browser::Memory::From(base);
  const auto read = [&](uint32_t address) {
    uint32_t value = 0;
    memory.Read(context, address, &value, 4);
    return value;
  };
  const uint32_t slot = read(context.fs_base + 4) + read(0x12100) * 4;
  const uint32_t data = read(slot);
  const uint32_t value = read(data) + 1;
  memory.Write(context, data, &value, 4);
  context.eax = value;
  context.esp += 4;
};
const auto rejects = [](const auto& operation) {
  bool failed = false;
  try { operation(); } catch (const std::runtime_error&) { failed = true; }
  assert(failed);
};
}

int main() {
  auto prepared = browser::PrepareImage(fixture(), layout);
  assert(prepared->image.entry == layout.entry);
  assert(scalar(*prepared, 0x10000) == 0x48454258);
  assert(scalar(*prepared, 0x11000) == 0x12345678);
  assert(scalar(*prepared, 0x12100) == 0);
  assert(scalar(*prepared, 0x12200) == 0);
  assert(scalar(*prepared, 0x10258) == 1);
  assert(scalar(*prepared, 0x10290) == 1);
  assert(scalar(*prepared, 0x102c8) == 0);
  assert(scalar(*prepared, 0x12040) == 0x80000001);
  assert(prepared->imports.size() == 2);
  assert(prepared->imports[0].slot == 0x12040 && prepared->imports[0].ordinal == 1);
  assert(prepared->imports[1].slot == 0x12044 && prepared->imports[1].ordinal == 302);
  assert((prepared->tlsTemplate == std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0, 0}));
  assert((prepared->tlsCallbacks == std::vector<uint32_t>{0x11010}));
  assert(!prepared->memory->IsCommitted(0x13000));
  rejects([&] { scalar(*prepared, 0x13000); });
  prepared->memory->Write({}, 0x12080, "abcd", 4);
  auto second = browser::PrepareImage(fixture(), layout);
  assert(scalar(*second, 0x12080) == 0x44332211);
  assert(prepared->tlsTemplate[0] == 0x11);
  for (const auto& wrong : std::array<browser::ImageLayout, 3>{
    browser::ImageLayout{0x11000, 0x13001, 0x11000},
    browser::ImageLayout{0x10000, 0x14000, 0x11000},
    browser::ImageLayout{0x10000, 0x13001, 0x11010},
  }) { rejects([&] { browser::PrepareImage(fixture(), wrong); }); }
  rejects([&] { browser::PrepareImage({}, layout); });
  auto bytes = fixture();
  put(bytes, 0x104, 0x10001);
  rejects([&] { browser::PrepareImage(bytes, layout); });
  bytes = fixture();
  put(bytes, 0x240, xbe::kExecutable);
  rejects([&] { browser::PrepareImage(bytes, layout); });
  bytes = fixture();
  put(bytes, 0x278, xbe::kWritable);
  rejects([&] { browser::PrepareImage(bytes, layout); });
  bytes = fixture();
  put(bytes, 0x610, 0);
  put(bytes, 0x604, 0x12080);
  put(bytes, 0x60c, 0);
  auto emptyTls = browser::PrepareImage(bytes, layout);
  assert(emptyTls->tlsTemplate.empty() && emptyTls->tlsCallbacks.empty());
  bytes = fixture();
  put(bytes, 0x12c, 0);
  auto noTls = browser::PrepareImage(bytes, layout);
  assert(noTls->tlsTemplate.empty() && noTls->tlsCallbacks.empty());
  bytes = fixture();
  put(bytes, 0x10c, 64u << 20);
  rejects([&] { browser::PrepareImage(bytes, {layout.base, layout.base + (64u << 20), layout.entry}); });
  assert(scalar(*second, 0x12080) == 0x44332211);
  const auto checkUnbound = [](browser::PreparedImage& image) {
    assert(!image.importsBound && image.boundImports.empty());
    assert(scalar(image, 0x12040) == 0x80000001);
    assert(scalar(image, 0x12044) == 0x8000012e);
  };
  {
    browser::Runtime runtime({}, {});
    auto image = browser::PrepareImage(fixture(), layout);
    const auto coverage = browser::InspectImageImports(*image, runtime, {});
    assert(coverage.size() == 2 && coverage[0].ordinal == 1 && coverage[1].ordinal == 302);
    for (const auto& import : coverage) { assert(import.kind == browser::ImportKind::Missing && !import.target); }
    checkUnbound(*image);
    rejects([&] { browser::BindImageImports(*image, runtime, {}); });
    checkUnbound(*image);
    browser::PreparedImage absent;
    rejects([&] { browser::BindImageImports(absent, runtime, {}); });
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    assert(runtime.KernelTarget(1) == 0xfe000004);
    assert(runtime.KernelTarget(0) == 0 && runtime.KernelTarget(400) == 0);
    assert(runtime.KernelTarget(302) == 0);
    auto image = browser::PrepareImage(fixture(), layout);
    image->memory->Ensure(0x20000, 4096);
    const uint32_t data = 0x1234abcd;
    image->memory->Write({}, 0x20004, &data, 4);
    const std::array exports{browser::KernelDataExport{302, 0x20004, 4}};
    const auto coverage = browser::InspectImageImports(*image, runtime, exports);
    assert(coverage.size() == 2);
    assert(coverage[0].kind == browser::ImportKind::Function && coverage[0].target == 0xfe000004);
    assert(coverage[1].kind == browser::ImportKind::Data && coverage[1].target == 0x20004);
    checkUnbound(*image);
    browser::BindImageImports(*image, runtime, exports);
    assert(image->importsBound && image->boundImports.size() == 2);
    assert(image->boundImports[0].slot == 0x12040 && image->boundImports[0].ordinal == 1);
    assert(image->boundImports[0].kind == browser::ImportKind::Function);
    assert(image->boundImports[0].target == 0xfe000004);
    assert(image->boundImports[1].kind == browser::ImportKind::Data);
    assert(image->boundImports[1].target == 0x20004);
    assert(scalar(*image, 0x12048) == 0);
    X86Context context{};
    context.host = &runtime;
    runtime.Invoke(context, image->memory->Base(), scalar(*image, 0x12040));
    assert(context.eax == 0x76543210);
    assert(scalar(*image, scalar(*image, 0x12044)) == data);
    assert(image->image.U32(0x640) == 0x80000001);
    rejects([&] { browser::BindImageImports(*image, runtime, exports); });
    assert(scalar(*image, 0x12044) == 0x20004);
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    auto image = browser::PrepareImage(fixture(), layout);
    const auto coverage = browser::InspectImageImports(*image, runtime, {});
    assert(coverage.size() == 2);
    assert(coverage[0].ordinal == 1 && coverage[0].kind == browser::ImportKind::Function);
    assert(coverage[1].ordinal == 302 && coverage[1].kind == browser::ImportKind::Missing && !coverage[1].target);
    checkUnbound(*image);
    bool missing = false;
    try { browser::BindImageImports(*image, runtime, {}); } catch (const browser::Fault& fault) {
      missing = fault.address == 302 && std::string(fault.what()).find("unavailable image import ordinal") == 0;
    }
    assert(missing);
    checkUnbound(*image);
    runtime.RegisterKernel(302, service);
    browser::BindImageImports(*image, runtime, {});
    assert(scalar(*image, 0x12044) == 0xfe0004b8);
  }
  for (const auto& exports : std::vector<std::vector<browser::KernelDataExport>>{
    {{302, 0, 4}}, {{302, 0x20000, 0}}, {{302, 0x20fff, 2}}, {{302, 0xfffffff0, 32}},
    {{302, 0x21000, 4}}, {{302, 0x12044, 4}}, {{302, 0x12048, 4}},
    {{302, 0x20000, 4}, {302, 0x20004, 4}}, {{1, 0x20000, 4}, {302, 0x20004, 4}},
    {{0, 0x20000, 4}}, {{400, 0x20000, 4}},
  }) {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    auto image = browser::PrepareImage(fixture(), layout);
    image->memory->Ensure(0x20000, 4096);
    rejects([&] { browser::BindImageImports(*image, runtime, exports); });
    checkUnbound(*image);
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    runtime.RegisterKernel(302, service);
    auto image = browser::PrepareImage(fixture(), layout);
    const uint32_t changed = 0xdeadbeef;
    image->memory->Write({}, 0x12044, &changed, 4);
    rejects([&] { browser::BindImageImports(*image, runtime, {}); });
    assert(scalar(*image, 0x12040) == 0x80000001);
    assert(scalar(*image, 0x12044) == changed);
    assert(!image->importsBound && image->boundImports.empty());
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    runtime.RegisterKernel(302, service);
    auto image = browser::PrepareImage(fixture(), layout);
    image->memory->Decommit(0x12000, 4096);
    rejects([&] { browser::BindImageImports(*image, runtime, {}); });
    assert(!image->importsBound && image->boundImports.empty());
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    auto bytes = fixture();
    put(bytes, 0x644, 0x80000001);
    auto image = browser::PrepareImage(bytes, layout);
    browser::BindImageImports(*image, runtime, {});
    assert(scalar(*image, 0x12040) == scalar(*image, 0x12044));
    assert(image->boundImports.size() == 2);
  }
  {
    browser::Runtime runtime({}, {});
    auto bytes = fixture();
    put(bytes, 0x640, 0);
    auto image = browser::PrepareImage(bytes, layout);
    browser::BindImageImports(*image, runtime, {});
    assert(image->importsBound && image->boundImports.empty());
    assert(scalar(*image, 0x12040) == 0);
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    runtime.RegisterKernel(302, service);
    auto image = browser::PrepareImage(fixture(), layout);
    const uint32_t changed = 0x80000002;
    image->memory->Write({}, 0x12048, &changed, 4);
    rejects([&] { browser::BindImageImports(*image, runtime, {}); });
    checkUnbound(*image);
    auto fresh = browser::PrepareImage(fixture(), layout);
    runtime.Cancel();
    rejects([&] { browser::BindImageImports(*fresh, runtime, {}); });
    checkUnbound(*fresh);
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    auto image = browser::PrepareImage(fixture(), layout);
    const std::vector<browser::KernelDataExport> exports(400, {302, 0x20000, 4});
    rejects([&] { browser::BindImageImports(*image, runtime, exports); });
    checkUnbound(*image);
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.image.kernel_thunk = 0x80000000;
    image.imports = {{0x80000000, 1}, {0x80000004, 302}};
    const std::array<uint32_t, 3> original{0x80000001, 0x8000012e, 0};
    image.memory->Write({}, 0x80000000, original.data(), sizeof(original));
    const std::array exports{browser::KernelDataExport{302, 0xf0000004, 4}};
    rejects([&] { browser::BindImageImports(image, runtime, exports); });
    assert(scalar(image, 0x80000000) == original[0]);
    assert(scalar(image, 0x80000004) == original[1]);
    assert(!image.importsBound && image.boundImports.empty());
  }
  for (uint32_t n = 0; n < 3; ++n) {
    auto bytes = fixture();
    bytes.resize(0x5600);
    put(bytes, 0x11c, 2);
    put(bytes, 0x12c, 0);
    put(bytes, 0x10c, 0x7000);
    put(bytes, 0x280, 0x5000);
    put(bytes, 0x288, 0x5000);
    for (uint32_t n = 0; n < 4095; ++n) { put(bytes, 0x640 + n * 4, 0x80000001); }
    put(bytes, 0x640 + 4094 * 4, 0x8000012e);
    put(bytes, 0x640 + 4095 * 4, 0);
    auto image = browser::PrepareImage(bytes, {0x10000, 0x17000, 0x11000});
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    if (n != 1) { runtime.RegisterKernel(302, service); }
    if (n == 2) { image->memory->Decommit(0x16000, 4096); }
    if (n) {
      rejects([&] { browser::BindImageImports(*image, runtime, {}); });
      assert(scalar(*image, 0x12040) == 0x80000001);
      assert(!image->importsBound && image->boundImports.empty());
    } else {
      browser::BindImageImports(*image, runtime, {});
      assert(image->boundImports.size() == 4095);
      assert(scalar(*image, 0x12040) == 0xfe000004);
      assert(scalar(*image, 0x12040 + 4094 * 4) == 0xfe0004b8);
      assert(scalar(*image, 0x12040 + 4095 * 4) == 0);
    }
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    runtime.RegisterKernel(302, service);
    auto image = browser::PrepareImage(fixture(), layout);
    rejects([&] { browser::PrepareImageThread(*image, runtime, 0x10, 0x10000); });
    browser::BindImageImports(*image, runtime, {});
    const auto first = browser::PrepareImageThread(*image, runtime, 0x10, 0);
    assert(image->threads.size() == 1);
    assert(first.id == 0x10 && first.context.host == &runtime);
    assert(first.context.fs_base == first.kpcr && first.context.esp % 16 == 0);
    assert(first.context.esp >= first.stackLimit && first.context.esp < first.tls);
    assert(first.context.fcw == 0x27f && first.context.mxcsr == 0x1f80);
    assert(first.context.eax == 0 && first.context.thread_handle == 0);
    assert(first.tlsData == first.tls + 4 && first.stackTop - first.tls == 20);
    assert(scalar(*image, first.tls) == first.tlsData);
    assert(scalar(*image, first.tlsData) == 0x44332211);
    assert(scalar(*image, first.tlsData + 4) == 0);
    assert(first.stackTop + scalar(*image, 0x12100) * 4 == first.tls);
    assert(scalar(*image, first.kpcr) == UINT32_MAX);
    assert(scalar(*image, first.kpcr + 4) == first.stackTop);
    assert(scalar(*image, first.kpcr + 8) == first.stackLimit);
    assert(scalar(*image, first.kpcr + 0x18) == first.kpcr);
    assert(scalar(*image, first.kpcr + 0x1c) == first.kpcr);
    assert(scalar(*image, first.kpcr + 0x20) == first.kpcr + 0x28);
    assert(scalar(*image, first.kpcr + 0x28) == first.ethread);
    assert(scalar(*image, first.ethread) == 0x00800006);
    assert(scalar(*image, first.ethread + 0x1c) == first.stackTop);
    assert(scalar(*image, first.ethread + 0x20) == first.stackLimit);
    assert(scalar(*image, first.ethread + 0x28) == first.tls);
    assert(scalar(*image, first.ethread + 0x12c) == first.id);
    assert(!image->memory->IsCommitted(first.kpcr - 4096));
    assert(!image->memory->IsCommitted(first.stackLimit - 4096));
    assert(first.stackTop - first.stackLimit >= 0x10000 + 20);
    const uint32_t changed = 0xaabbccdd;
    image->memory->Write({}, first.tlsData, &changed, 4);
    const auto backing = image->memory->BackingBytes();
    rejects([&] { browser::PrepareImageThread(*image, runtime, 0x10, 0x10000); });
    rejects([&] { browser::PrepareImageThread(*image, runtime, 0, 0x10000); });
    rejects([&] { browser::PrepareImageThread(*image, runtime, 0x14, UINT32_MAX); });
    assert(image->threads.size() == 1 && image->memory->BackingBytes() == backing);
    {
      auto lease = image->memory->Borrow();
      rejects([&] { browser::PrepareImageThread(*image, runtime, 0x14, 0x10000); });
    }
#ifndef __EMSCRIPTEN__
    runtime.Launch(first.context, image->memory->Base(), [](X86Context& context, uint8_t*) {
      auto& owner = browser::Runtime::From(context);
      const auto sequence = owner.Sequence();
      owner.Wait(context, sequence, std::chrono::seconds(1));
    });
    rejects([&] { browser::PrepareImageThread(*image, runtime, 0x14, 0x10000); });
    runtime.Wake();
    assert(!runtime.Join());
#endif
    assert(image->threads.size() == 1 && image->memory->BackingBytes() == backing);
    const auto second = browser::PrepareImageThread(*image, runtime, 0x14, 0x11000);
    assert(image->threads.size() == 2 && second.kpcr != first.kpcr);
    assert(scalar(*image, second.tlsData) == 0x44332211);
    assert(scalar(*image, first.tlsData) == changed);
    assert(second.stackTop + scalar(*image, 0x12100) * 4 == second.tls);
    auto context = second.context;
    assert(runtime.Call(context, image->memory->Base(), runtime.HostThunk(tlsService), {}) == 0x44332212);
    assert(context.esp == second.context.esp);
    assert(scalar(*image, second.tlsData) == 0x44332212 && scalar(*image, first.tlsData) == changed);
    assert(image->tlsCallbacks.size() == 1 && image->image.U32(0x700) == 0xabcdef01);
    rejects([&] { browser::PrepareImageThread(*image, runtime, 0x18, 0x10000); });
  }
  for (uint32_t n = 0; n < 2; ++n) {
    auto bytes = fixture();
    if (n) {
      put(bytes, 0x604, 0x12080);
      put(bytes, 0x610, 0);
    } else { put(bytes, 0x12c, 0); }
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    runtime.RegisterKernel(302, service);
    auto image = browser::PrepareImage(bytes, layout);
    browser::BindImageImports(*image, runtime, {});
    const auto thread = browser::PrepareImageThread(*image, runtime, 0x10, 0x10000);
    if (n) {
      assert(thread.stackTop - thread.tls == 4);
      assert(scalar(*image, thread.tls) == thread.tlsData);
      assert(scalar(*image, 0x12100) == UINT32_MAX);
    } else {
      assert(thread.tls == thread.stackTop && thread.tlsData == 0);
      assert(scalar(*image, 0x12100) == 0);
    }
  }
  {
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(1, service);
    runtime.RegisterKernel(302, service);
    auto bytes = fixture();
    put(bytes, 0x610, (1u << 20) - 4);
    auto image = browser::PrepareImage(bytes, layout);
    browser::BindImageImports(*image, runtime, {});
    const auto thread = browser::PrepareImageThread(*image, runtime, 0x10, 16u << 20);
    assert(thread.stackTop - thread.tls == (1u << 20) + 4);
    assert(scalar(*image, thread.tlsData) == 0x44332211);
    assert(scalar(*image, thread.tlsData + (1u << 20) - 4) == 0);
    const uint32_t changed = 7;
    image->memory->Write({}, 0x12100, &changed, 4);
    const auto backing = image->memory->BackingBytes();
    rejects([&] { browser::PrepareImageThread(*image, runtime, 0x14, 0x10000); });
    assert(image->threads.size() == 1 && image->memory->BackingBytes() == backing);
    assert(scalar(*image, 0x12100) == changed);
  }
  for (uint32_t n = 0; n < 6; ++n) {
    browser::Runtime runtime({}, {});
    browser::Runtime wrong({}, {});
    runtime.RegisterKernel(1, service);
    runtime.RegisterKernel(302, service);
    auto bytes = fixture();
    if (n == 4) { put(bytes, 0x608, 0x12040); }
    auto image = browser::PrepareImage(bytes, layout);
    browser::BindImageImports(*image, runtime, {});
    if (n == 0) { runtime.Cancel(); }
    if (n == 2) { image->memory->Decommit(0x12000, 4096); }
    if (n == 3) { image->memory->Map(0xd0000000, 4096); }
    if (n == 5) { image->memory->Ensure(0x20000000, 47u << 20); }
    const auto backing = image->memory->BackingBytes();
    rejects([&] { browser::PrepareImageThread(*image, n == 1 ? wrong : runtime, 0x10, n == 5 ? 2u << 20 : 0x10000); });
    assert(image->threads.empty() && image->memory->BackingBytes() == backing);
    if (n != 2 && n != 4) { assert(scalar(*image, 0x12100) == 0); }
  }
  std::puts("image-contracts-ok");
}
