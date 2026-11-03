#include "browser/platform-profile.h"
#include "browser/kernel-provider.h"
#include "cpu/x86_ops.h"
#include <array>
#include <cstdio>
#include <string_view>

X86_EXTERN_FUNC(sub_000531AC);
X86_EXTERN_FUNC(sub_00053213);
X86_EXTERN_FUNC(sub_00059F30);
X86_EXTERN_FUNC(sub_000677A4);

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto refuse = [](const auto& operation) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, "invalid signature export operation succeeded");
};

struct Fixture {
  browser::Runtime runtime{{}, {}};
  browser::PreparedImage image;
  Fixture() {
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 2> imports = {0x80000145, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 325}};
  }
  browser::KernelDataExport Install() { return browser::InstallSignatureKey(runtime, image); }
  void RejectUnchanged() {
    const auto backing = image.memory->BackingBytes();
    const auto page = image.memory->QueryPages(0xc0004000);
    auto* owner = image.diagnosticRuntime;
    refuse([&] { Install(); });
    check(image.memory->BackingBytes() == backing && image.diagnosticRuntime == owner &&
      image.memory->QueryPages(0xc0004000).mapped == page.mapped,
      "rejected signature export changed backing or ownership");
  }
};

const auto storageContracts = [] {
  Fixture first;
  auto& memory = *first.image.memory;
  memory.Ensure(0xc0003000, 4096);
  memory.Ensure(0xc0005000, 4096);
  constexpr uint32_t sentinel = 0xaabbccdd;
  memory.Write({}, 0xc0003ffc, &sentinel, 4);
  memory.Write({}, 0xc0005000, &sentinel, 4);
  const auto backing = memory.BackingBytes();
  const auto data = first.Install();
  check(data.ordinal == 325 && data.address == 0xc0004000 && data.size == 16,
    "signature export is not the exact sixteen byte data ABI");
  const auto page = memory.QueryPages(data.address);
  check(page.mapped && page.committed && page.mappingBase == data.address && page.end == data.address + 4096 &&
    memory.BackingBytes() == backing + 4097, "signature export lacks fresh dedicated storage");
  std::array<uint8_t, 4096> observed{};
  memory.Read({}, data.address, observed.data(), observed.size());
  check(observed == std::array<uint8_t, 4096>{}, "initial signature export or padding is not zero filled");
  check(!memory.QueryPages(0xc1145000).mapped && !first.runtime.KernelTarget(325),
    "signature installation broadly committed a diagnostic guard or registered a function");
  for (const uint32_t address : {0xc0003ffcu, 0xc0005000u}) {
    uint32_t value = 0;
    memory.Read({}, address, &value, 4);
    check(value == sentinel, "signature write exceeded its owned page");
  }
  memory.Write({}, data.address, &sentinel, 4);
  {
    Fixture second;
    const auto isolated = second.Install();
    std::array<uint8_t, 16> bytes{};
    second.image.memory->Read({}, isolated.address, bytes.data(), bytes.size());
    check(bytes == std::array<uint8_t, 16>{}, "independent image inherited signature contents");
    uint32_t value = 0;
    memory.Read({}, data.address, &value, 4);
    check(value == sentinel, "second image reset the first signature export");
  }
  first.RejectUnchanged();
  browser::Runtime foreign({}, {});
  refuse([&] { browser::BindImageImports(first.image, foreign, std::span(&data, 1)); });
  check(!first.image.importsBound, "foreign runtime bound signature storage");
  browser::BindImageImports(first.image, first.runtime, std::span(&data, 1));
  check(first.image.boundImports.size() == 1 && first.image.boundImports[0].kind == browser::ImportKind::Data &&
    first.image.boundImports[0].target == data.address && !first.runtime.KernelTarget(325),
    "signature import did not bind as owned data");
  uint32_t target = 0;
  memory.Read({}, 0x10000, &target, 4);
  check(target == data.address, "signature import slot has the wrong target");
  refuse([&] { first.runtime.RegisterKernel(325, [](X86Context&, uint8_t*) {}); });
  first.RejectUnchanged();
  first.runtime.Stop();
  first.RejectUnchanged();
};

const auto rejectionContracts = [] {
  for (uint32_t n = 0; n < 11; ++n) {
    Fixture fixture;
    browser::Runtime foreign({}, {});
    if (n == 0) { fixture.runtime.Cancel(); }
    if (n == 1) { fixture.image.importsBound = true; }
    if (n == 2) { fixture.image.boundRuntime = &fixture.runtime; }
    if (n == 3) { fixture.image.boundRuntime = &foreign; }
    if (n == 4) { fixture.image.diagnosticRuntime = &foreign; }
    if (n == 5) { fixture.runtime.RegisterKernel(325, [](X86Context&, uint8_t*) {}); }
    if (n == 6 || n == 7) { browser::DeclareDiagnosticImport(fixture.image, fixture.runtime, 325, n == 6); }
    if (n == 8) { fixture.runtime.PublishImageMemory(*fixture.image.memory, fixture.runtime.KernelRevision(), [] {}); }
    if (n == 9) { fixture.image.diagnosticImports.push_back({0x10000, 349, browser::ImportKind::FunctionTrap, 0}); }
    if (n == 10) {
      browser::Memory other;
      fixture.runtime.PublishImageMemory(other, fixture.runtime.KernelRevision(), [] {});
      fixture.RejectUnchanged();
      continue;
    }
    fixture.RejectUnchanged();
  }
  for (const bool committed : {false, true}) {
    Fixture fixture;
    fixture.image.memory->Map(0xc0004000, 4096);
    if (committed) {
      fixture.image.memory->Commit(0xc0004000, 4096);
      constexpr uint32_t sentinel = 0x12345678;
      fixture.image.memory->Write({}, 0xc0004000, &sentinel, 4);
    }
    fixture.RejectUnchanged();
    if (committed) {
      uint32_t observed = 0;
      fixture.image.memory->Read({}, 0xc0004000, &observed, 4);
      check(observed == 0x12345678, "conflicting signature mapping was overwritten");
    }
  }
  {
    Fixture fixture;
    auto lease = fixture.image.memory->Borrow();
    fixture.RejectUnchanged();
  }
  {
    Fixture fixture;
    browser::Memory other;
    auto lease = other.Borrow();
    fixture.RejectUnchanged();
  }
  {
    Fixture fixture;
    fixture.runtime.Launch({}, fixture.image.memory->Base(), [&](X86Context&, uint8_t*) { fixture.RejectUnchanged(); });
    if (const auto failure = fixture.runtime.Join()) { std::rethrow_exception(failure); }
  }
  {
    Fixture fixture;
    fixture.image.memory->Map(0x20000000, (48u << 20) - 0x8000);
    fixture.RejectUnchanged();
  }
  {
    Fixture fixture;
    fixture.image.memory.reset();
    refuse([&] { fixture.Install(); });
  }
  {
    Fixture fixture;
    const auto drive = browser::InstallVirtualDriveKey(fixture.runtime, fixture.image);
    const auto profile = browser::InstallPlatformProfile(fixture.runtime, fixture.image);
    const auto signature = fixture.Install();
    check(drive.ordinal == 323 && profile.size() == 6 && signature.ordinal == 325 &&
      fixture.image.diagnosticRuntime == &fixture.runtime, "signature installer rejected its own platform session");
  }
};

const auto knownAnswers = [] {
  constexpr std::array<std::string_view, 3> payloads = {"", "abc", "The quick brown fox jumps over the lazy dog"};
  constexpr std::array<std::array<uint8_t, 20>, 3> expected = {{
    {0xfb,0xdb,0x1d,0x1b,0x18,0xaa,0x6c,0x08,0x32,0x4b,0x7d,0x64,0xb7,0x1f,0xb7,0x63,0x70,0x69,0x0e,0x1d},
    {0x9b,0x4a,0x91,0x8f,0x39,0x8d,0x74,0xd3,0xe3,0x67,0x97,0x0a,0xba,0x3c,0xbe,0x54,0xe4,0xd2,0xb5,0xd9},
    {0x2b,0xa7,0xf7,0x07,0xad,0x5f,0x18,0x7c,0x41,0x2d,0xe3,0x10,0x65,0x83,0xc3,0x11,0x1d,0x66,0x8d,0xe8}
  }};
  const std::array<X86FuncMapping, 4> mappings = {{{0x531ac, sub_000531AC}, {0x53213, sub_00053213},
    {0x59f30, sub_00059F30}, {0x677a4, sub_000677A4}}};
  browser::Runtime runtime(mappings, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  auto& memory = *image.memory;
  memory.Ensure(0x10000, 0x4000);
  image.image.kernel_thunk = 0x10000;
  constexpr std::array<uint32_t, 2> imports = {0x80000145, 0};
  memory.Write({}, 0x10000, imports.data(), sizeof(imports));
  image.imports = {{0x10000, 325}};
  browser::InstallKernelProvider(runtime, memory);
  const auto data = browser::InstallSignatureKey(runtime, image);
  browser::BindImageImports(image, runtime, std::span(&data, 1));
  const auto thread = browser::PrepareImageThread(image, runtime, 0x10, 0x10000);
  runtime.Launch(thread.context, memory.Base(), [&](X86Context& context, uint8_t* base) {
    for (size_t n = 0; n < payloads.size(); ++n) {
      uint32_t key = 0;
      memory.Read(context, 0x10000, &key, 4);
      memory.Write(context, 0x11000, payloads[n].data(), payloads[n].size());
      memory.Write(context, 0x12020, expected[n].data(), expected[n].size());
      const auto stack = context.esp;
      runtime.Call(context, base, 0x531ac, {key, 16, 0x13000});
      runtime.Call(context, base, 0x59f30, {0x13000, 0x11000, uint32_t(payloads[n].size())});
      runtime.Call(context, base, 0x53213, {0x13000, key, 16, 0x12000});
      check(context.esp == stack, "generated signature calculation changed the caller stack");
      std::array<uint8_t, 20> digest{};
      memory.Read(context, 0x12000, digest.data(), digest.size());
      check(digest == expected[n], "generated mode-zero HMAC failed the public known answer");
      const auto compare = [&] {
        const auto result = runtime.Call(context, base, 0x677a4, {0x12000, 0x12020, 20});
        check(context.esp == stack, "generated comparison changed the caller stack");
        return result == 0;
      };
      check(compare(), "matching public trailer failed real guest comparison");
      for (uint32_t offset : {0u, 9u, 19u}) {
        auto mismatch = expected[n];
        mismatch[offset] ^= 1;
        memory.Write(context, 0x12020, mismatch.data(), mismatch.size());
        check(!compare(), "mismatching public trailer passed real guest comparison");
      }
    }
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
};

int main() {
  storageContracts();
  rejectionContracts();
  knownAnswers();
  std::puts("signature-export-contracts-ok");
}
