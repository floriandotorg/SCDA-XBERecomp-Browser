#include "browser/section-provider.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#ifndef __EMSCRIPTEN__
#include <fstream>
#include <iterator>
#endif
#include "section-fixture.h"

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto put = [](std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value) {
  for (uint32_t n = 0; n < 4; ++n) { bytes.at(offset + n) = uint8_t(value >> (n * 8)); }
};
const auto fixture = [] {
  std::vector<uint8_t> bytes(0x2800);
  std::memcpy(bytes.data(), "XBEH", 4);
  for (const auto& [offset, value] : std::vector<std::pair<uint32_t, uint32_t>>{
    {0x104, 0x10000}, {0x108, 0x1000}, {0x10c, 0x6000}, {0x118, 0x10200},
    {0x11c, 4}, {0x120, 0x10240}, {0x128, 0x11000 ^ xbe::kRetailEntryKey},
    {0x158, 0x11040 ^ xbe::kRetailThunkKey}, {0x200, 12},
    {0x240, xbe::kExecutable | xbe::kPreload}, {0x244, 0x11000}, {0x248, 0x800},
    {0x24c, 0x1000}, {0x250, 0x800}, {0x254, 0x10400}, {0x25c, 0x10480}, {0x260, 0x10480},
    {0x278, xbe::kWritable}, {0x27c, 0x11800}, {0x280, 0x2800},
    {0x284, 0x1800}, {0x288, 0x800}, {0x28c, 0x10400}, {0x294, 0x10480}, {0x298, 0x10482},
    {0x2b0, xbe::kWritable}, {0x2b4, 0x14000}, {0x2b8, 0x800},
    {0x2bc, 0x2000}, {0x2c0, 0x400}, {0x2c4, 0x10400}, {0x2cc, 0x10484}, {0x2d0, 0x10484},
    {0x2e8, xbe::kWritable | xbe::kPreload}, {0x2ec, 0x14800}, {0x2f0, 0x800},
    {0x2f4, 0x2400}, {0x2f8, 0x400}, {0x2fc, 0x10400}, {0x304, 0x10484}, {0x308, 0x10484},
    {0x1040, 0x80000147}, {0x1044, 0x80000148}, {0x1800, 0x44332211},
    {0x2000, 0x88776655}, {0x2400, 0xabcdef01}
  }) { put(bytes, offset, value); }
  return bytes;
};
const auto scalar = [](browser::Memory& memory, uint32_t address) {
  uint32_t value = 0;
  memory.Read({}, address, &value, 4);
  return value;
};
const auto counter = [](browser::Memory& memory, uint32_t address) {
  uint16_t value = 0;
  memory.Read({}, address, &value, 2);
  return value;
};
const auto rejected = [](const auto& operation) {
  bool refused = false;
  try { operation(); } catch (const browser::Fault&) { refused = true; }
  check(refused, "invalid section operation returned success");
};

int main() try {
#ifndef __EMSCRIPTEN__
  {
    std::ifstream file("assets/default.xbe", std::ios::binary);
    check(bool(file), "actual authenticated XBE prerequisite unavailable: assets/default.xbe");
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{file}, {});
    xbe::Image original;
    std::string error;
    check(original.Parse(bytes, error), "actual XBE metadata is invalid");
    auto image = browser::PrepareImage(std::move(bytes), {original.base, original.base + original.image_size, original.entry});
    auto& memory = *image->memory;
    browser::Runtime runtime({}, {});
    browser::InstallSectionProvider(runtime, *image);
    const auto source = image->AuthenticatedSource();
    const uint32_t actualHeader = source->U32(0x120) + 30 * 56;
    check(actualHeader == 0x10a00 && source->sections[30].va == 0x6c89e0 && source->sections[30].vsize == 0x2800 &&
      source->sections[30].name == "$$XTIMAGE", "actual observed section identity changed");
    for (const auto& imported : image->imports) { browser::DeclareDiagnosticImport(*image, runtime, imported.ordinal, false); }
    browser::BindImageImports(*image, runtime, {});
    memory.Ensure(0x20000000, 4096);
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x98760000;
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      const auto invoke = [&](uint32_t ordinal, uint32_t header) {
        ctx.esp = 0x20000f00;
        const std::array<uint32_t, 2> frame = {0x12345678, header};
        memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
        runtime.Invoke(ctx, base, runtime.KernelTarget(ordinal));
        check(ctx.esp == 0x20000f08 && ctx.eax == 0, "actual section ABI changed");
      };
      invoke(327, actualHeader + 56);
      std::array<uint8_t, 0x9e0> prefix{}, prefixAfter{};
      std::array<uint8_t, 0xe20> suffix{}, suffixAfter{};
      memory.Read(ctx, 0x6c8000, prefix.data(), prefix.size());
      memory.Read(ctx, 0x6cb1e0, suffix.data(), suffix.size());
      for (uint32_t n = 0; n < 3; ++n) {
        invoke(327, actualHeader);
        std::array<uint8_t, 0x2800> loaded{};
        memory.Read(ctx, 0x6c89e0, loaded.data(), loaded.size());
        check(std::equal(loaded.begin(), loaded.end(), source->file.begin() + source->sections[30].raw),
          "actual reload lost authenticated section bytes");
        const uint32_t changed = 0xdeadbeef;
        memory.Write(ctx, 0x6c89e0, &changed, 4);
        invoke(328, actualHeader);
        check(!memory.IsCommitted(0x6c9000) && !memory.IsCommitted(0x6ca000) && memory.IsCommitted(0x6c8000) &&
          memory.IsCommitted(0x6cb000), "actual unload lost exclusive or shared page ownership");
        memory.Read(ctx, 0x6c8000, prefixAfter.data(), prefixAfter.size());
        memory.Read(ctx, 0x6cb1e0, suffixAfter.data(), suffixAfter.size());
        check(prefix == prefixAfter && suffix == suffixAfter, "actual unload or reload corrupted shared neighbors");
      }
      invoke(328, actualHeader + 56);
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    std::puts("actual-section-0x10a00-lifecycle-ok");
  }
#endif
  for (const uint32_t ordinal : {327u, 328u}) {
    auto image = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(ordinal, [](X86Context&, uint8_t*) {});
    rejected([&] { browser::InstallSectionProvider(runtime, *image); });
    check(counter(*image->memory, 0x10480) == 0 && !runtime.KernelTarget(ordinal == 327 ? 328 : 327),
      "conflicting ordinal partially installed lifecycle provider");
  }
  {
    auto bytes = fixture();
    for (const auto& [offset, value] : std::vector<std::pair<uint32_t, uint32_t>>{
      {0x108, 0x800}, {0x128, 0x10800 ^ xbe::kRetailEntryKey}, {0x158, 0x10840 ^ xbe::kRetailThunkKey},
      {0x244, 0x10800}, {0x25c, 0x10486}, {0x260, 0x10486}
    }) { put(bytes, offset, value); }
    auto image = browser::PrepareImage(std::move(bytes), {0x10000, 0x16000, 0x10800});
    browser::Runtime runtime({}, {});
    browser::InstallSectionProvider(runtime, *image);
    browser::BindImageImports(*image, runtime, {});
    auto& memory = *image->memory;
    memory.Ensure(0x20000, 4096);
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x98760000;
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      std::array<uint8_t, 0x800> before{}, after{};
      memory.Read(ctx, 0x10000, before.data(), before.size());
      ctx.esp = 0x20f00;
      const std::array<uint32_t, 2> frame = {0x12345678, 0x10240};
      memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      runtime.Invoke(ctx, base, runtime.KernelTarget(328));
      memory.Read(ctx, 0x10000, after.data(), after.size());
      std::fill_n(before.begin() + 0x258, 4, 0);
      std::fill_n(before.begin() + 0x486, 2, 0);
      check(before == after && memory.IsCommitted(0x10000) && counter(memory, 0x10486) == 0,
        "unload decommitted shared immutable header backing");
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
  {
    auto image = browser::PrepareImage({std::begin(sharedFixture), std::end(sharedFixture)}, {0x10000, 0x13000, 0x11000});
    browser::Runtime runtime({}, {});
    browser::InstallSectionProvider(runtime, *image);
    check(counter(*image->memory, 0x10320) == 2 && counter(*image->memory, 0x10322) == 2,
      "shared JavaScript fixture lost valid preload section lifecycle");
  }
  {
    auto prepared = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    browser::PreparedImage counterfeit;
    counterfeit.image = prepared->image;
    counterfeit.memory = std::move(prepared->memory);
    browser::Runtime runtime({}, {});
    rejected([&] { browser::InstallSectionProvider(runtime, counterfeit); });
    check(!runtime.KernelTarget(327), "unowned source installed a section export");
  }
  for (const auto& [offset, value] : std::vector<std::pair<uint32_t, uint32_t>>{
    {0x294, 0}, {0x294, 0x10481}, {0x294, 0x10240}, {0x294, 0x10400},
    {0x294, 0x10200}, {0x294, 0x11000}, {0x294, 0x10482}, {0x290, 1}, {0x480, 1}
  }) {
    auto bytes = fixture();
    put(bytes, offset, value);
    auto image = browser::PrepareImage(std::move(bytes), {0x10000, 0x16000, 0x11000});
    browser::Runtime runtime({}, {});
    std::array<uint8_t, 4096> before{}, after{};
    image->memory->Read({}, 0x10000, before.data(), before.size());
    rejected([&] { browser::InstallSectionProvider(runtime, *image); });
    image->memory->Read({}, 0x10000, after.data(), after.size());
    check(before == after && !runtime.KernelTarget(327), "invalid metadata installation was not atomic");
  }
  {
    auto image = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    browser::Runtime runtime({}, {});
    browser::Memory foreign;
    runtime.PublishImageMemory(foreign, runtime.KernelRevision(), [] {});
    rejected([&] { browser::InstallSectionProvider(runtime, *image); });
    check(counter(*image->memory, 0x10480) == 0 && !runtime.KernelTarget(327),
      "foreign bound runtime changed section installation metadata");
  }
  for (uint32_t n = 0; n < 3; ++n) {
    auto image = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    auto& memory = *image->memory;
    browser::Runtime runtime({}, {});
    std::fill(image->image.file.begin() + 0x1800, image->image.file.begin() + 0x2000, 0x99);
    browser::InstallSectionProvider(runtime, *image);
    check(!memory.IsCommitted(0x12000) && !memory.IsCommitted(0x13000) && !memory.IsCommitted(0x15000),
      "unloaded exclusive pages remained committed");
    check(memory.IsCommitted(0x11000) && memory.IsCommitted(0x14000), "shared preload pages were decommitted");
    check(counter(memory, 0x10480) == 2 && counter(memory, 0x10482) == 0 && counter(memory, 0x10484) == 2,
      "preload head and tail counters were not initialized");
    check(runtime.KernelTarget(328) != 0, "XeUnloadSection export is absent");
    browser::BindImageImports(*image, runtime, {});
    memory.Ensure(0x20000, 4096);
    memory.Ensure(0x21000, 4096);
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x98760000;
    const auto invoke = [&](X86Context& ctx, uint8_t* base, uint32_t section, uint32_t stack = 0x20f00, uint32_t ordinal = 327) {
      ctx.esp = stack;
      ctx.eax = 0xfeedabcd;
      const std::array<uint32_t, 2> frame = {0x12345678, section};
      memory.Write(ctx, stack, frame.data(), sizeof(frame));
      runtime.Invoke(ctx, base, runtime.KernelTarget(ordinal));
      check(ctx.esp == stack + 8 && ctx.eax == 0, "section stdcall/status ABI is wrong");
    };
    const auto reject = [&](X86Context& ctx, uint8_t* base, uint32_t section, uint32_t ordinal = 327) {
      std::array<uint8_t, 4096> before{}, after{};
      memory.Read({}, 0x10000, before.data(), before.size());
      const bool committed = memory.IsCommitted(0x12000);
      rejected([&] { invoke(ctx, base, section, 0x20f00, ordinal); });
      memory.Read({}, 0x10000, after.data(), after.size());
      check(before == after && memory.IsCommitted(0x12000) == committed && ctx.esp == 0x20f00 && ctx.eax == 0xfeedabcd,
        "failed load changed metadata, commitment or ABI registers");
    };
    reject(context, memory.Base(), 0x10278);
    reject(context, memory.Base(), 0x10278, 328);
    std::atomic<uint32_t> ready = 0;
    std::atomic<bool> done = false;
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      memory.Copy(ctx, 0x20000, 0x10278, 56);
      for (const uint32_t address : {0u, 0x10279u, 0x10274u, 0x20000u, 0xfffffffcu}) {
        reject(ctx, base, address);
        reject(ctx, base, address, 328);
      }
      reject(ctx, base, 0x10278, 328);
      for (const uint32_t stack : {0x21ffcu, 0x15000u, 0xfffffffcu}) {
        ctx.esp = stack;
        ctx.eax = 0xfeedabcd;
        rejected([&] { runtime.Invoke(ctx, base, runtime.KernelTarget(327)); });
        rejected([&] { runtime.Invoke(ctx, base, runtime.KernelTarget(328)); });
        check(ctx.esp == stack && ctx.eax == 0xfeedabcd && scalar(memory, 0x10290) == 0,
          "invalid stack changed section state or ABI registers");
      }
      const std::array<uint32_t, 14> fields = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52};
      for (const uint32_t offset : fields) {
        const uint32_t original = scalar(memory, 0x10278 + offset);
        const uint32_t modified = original ^ 1;
        memory.Write({}, 0x10278 + offset, &modified, 4);
        reject(ctx, base, 0x10278);
        reject(ctx, base, 0x10278, 328);
        memory.Write({}, 0x10278 + offset, &original, 4);
      }
      const uint16_t wrongCounter = 3;
      const uint16_t correctCounter = 2;
      memory.Write({}, 0x10480, &wrongCounter, 2);
      reject(ctx, base, 0x10278);
      memory.Write({}, 0x10480, &correctCounter, 2);
      memory.Commit(0x12000, 4096);
      reject(ctx, base, 0x10278);
      memory.Decommit(0x12000, 4096);
      memory.Decommit(0x14000, 4096);
      reject(ctx, base, 0x10278);
      memory.Commit(0x14000, 4096);
      const uint32_t preloadNeighbor = 0xabcdef01;
      memory.Write({}, 0x14800, &preloadNeighbor, 4);
      {
        auto lease = memory.Borrow();
        reject(ctx, base, 0x10278);
      }
      auto alien = ctx;
      alien.fs_base += 4;
      reject(alien, base, 0x10278);
      browser::Runtime foreign({}, {});
      alien = ctx;
      alien.host = &foreign;
      reject(alien, base, 0x10278);
      reject(ctx, nullptr, 0x10278);
      std::fill(image->image.file.begin() + 0x1800, image->image.file.begin() + 0x2000, 0x77);
      invoke(ctx, base, 0x10278);
      check(memory.IsCommitted(0x12000) && memory.IsCommitted(0x13000), "load did not commit exclusive pages");
      check(scalar(memory, 0x11800) == 0x44332211 && scalar(memory, 0x11ffc) == 0 && scalar(memory, 0x13ffc) == 0,
        "load did not use retained authenticated bytes or zero the tail");
      check(scalar(memory, 0x11040) == runtime.KernelTarget(327), "load overwrote shared preload bytes");
      check(counter(memory, 0x10480) == 3 && counter(memory, 0x10482) == 1, "first load counters are wrong");
      const uint32_t changed = 0xaabbccdd;
      memory.Write({}, 0x11800, &changed, 4);
      invoke(ctx, base, 0x10278);
      check(scalar(memory, 0x11800) == changed && scalar(memory, 0x10290) == 2 && counter(memory, 0x10480) == 3,
        "repeat load recopied data or increased page counters");
      invoke(ctx, base, 0x102b0);
      check(scalar(memory, 0x14000) == 0x88776655 && counter(memory, 0x10484) == 4,
        "same page head/tail was not counted twice");
      const auto unload = [&](uint32_t section) { invoke(ctx, base, section, 0x20f00, 328); };
      unload(0x10278);
      check(scalar(memory, 0x10290) == 1 && scalar(memory, 0x11800) == changed && memory.IsCommitted(0x12000) &&
        counter(memory, 0x10480) == 3 && counter(memory, 0x10482) == 1, "nonfinal unload changed live data or page ownership");
      unload(0x10278);
      check(scalar(memory, 0x10290) == 0 && !memory.IsCommitted(0x12000) && !memory.IsCommitted(0x13000) &&
        memory.IsCommitted(0x11000) && counter(memory, 0x10480) == 2 && counter(memory, 0x10482) == 0,
        "final unload did not release only exclusive pages");
      check(scalar(memory, 0x11800) == changed && scalar(memory, 0x11040) == runtime.KernelTarget(327) &&
        memory.IsCommitted(0x20000), "unload destroyed shared bytes, import data or unrelated backing");
      reject(ctx, base, 0x10278, 328);
      for (uint32_t n = 0; n < 3; ++n) {
        invoke(ctx, base, 0x10278);
        check(scalar(memory, 0x11800) == 0x44332211 && scalar(memory, 0x13ffc) == 0 &&
          scalar(memory, 0x11040) == runtime.KernelTarget(327), "reload did not restore authenticated bytes and preserve neighbors");
        memory.Write({}, 0x11800, &changed, 4);
        unload(0x10278);
      }
      unload(0x102b0);
      check(counter(memory, 0x10484) == 2 && memory.IsCommitted(0x14000) && scalar(memory, 0x14800) == 0xabcdef01,
        "same-page unload lost its live shared neighbor");
      unload(0x102e8);
      check(counter(memory, 0x10484) == 0 && !memory.IsCommitted(0x14000), "last same-page owner did not decommit");
      invoke(ctx, base, 0x102e8);
      check(counter(memory, 0x10484) == 2 && scalar(memory, 0x14800) == 0xabcdef01,
        "preloaded section reload did not restore original data");
      invoke(ctx, base, 0x10278);
      {
        const std::array<uint32_t, 2> frame = {0x12345678, 0x10278};
        ctx.esp = 0x12ff0;
        ctx.eax = 0xfeedabcd;
        memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
        rejected([&] { runtime.Invoke(ctx, base, runtime.KernelTarget(328)); });
        check(ctx.esp == 0x12ff0 && ctx.eax == 0xfeedabcd && scalar(memory, 0x10290) == 1 &&
          memory.IsCommitted(0x12000) && counter(memory, 0x10480) == 3,
          "unload decommit invalidated its call frame or partially changed state");
      }
      {
        auto lease = memory.Borrow();
        reject(ctx, base, 0x10278, 328);
      }
      reject(alien, base, 0x10278, 328);
      reject(ctx, nullptr, 0x10278, 328);
      alien = ctx;
      alien.fs_base += 4;
      reject(alien, base, 0x10278, 328);
      const uint16_t badLiveCounter = 0;
      const uint16_t goodLiveCounter = 3;
      memory.Write({}, 0x10480, &badLiveCounter, 2);
      reject(ctx, base, 0x10278, 328);
      memory.Write({}, 0x10480, &goodLiveCounter, 2);
      invoke(ctx, base, 0x10278);
      ++ready;
      while (ready.load() < 2) {
        runtime.Checkpoint(ctx);
        std::this_thread::yield();
      }
      for (uint32_t n = 0; n < 100; ++n) {
        invoke(ctx, base, 0x10278);
        invoke(ctx, base, 0x10278, 0x20f00, 328);
      }
      done = true;
    });
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      ++ready;
      while (ready.load() < 2) {
        runtime.Checkpoint(ctx);
        std::this_thread::yield();
      }
      for (uint32_t n = 0; n < 100; ++n) {
        invoke(ctx, base, 0x10278, 0x21f00);
        invoke(ctx, base, 0x10278, 0x21f00, 328);
      }
      while (!done.load()) {
        runtime.Checkpoint(ctx);
        std::this_thread::yield();
      }
      check(scalar(memory, 0x10290) == 2 && counter(memory, 0x10480) == 3, "concurrent lifecycle lost refs or page ownership");
      invoke(ctx, base, 0x10278, 0x21f00, 328);
      invoke(ctx, base, 0x10278, 0x21f00, 328);
      check(!memory.IsCommitted(0x12000) && counter(memory, 0x10480) == 2,
        "concurrent references could not release exclusive backing");
      invoke(ctx, base, 0x10278, 0x21f00);
      runtime.Cancel();
      rejected([&] { invoke(ctx, base, 0x10278, 0x21f00); });
      rejected([&] { invoke(ctx, base, 0x10278, 0x21f00, 328); });
      check(scalar(memory, 0x10290) == 1 && memory.IsCommitted(0x12000), "cancelled lifecycle changed ownership");
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    rejected([&] { browser::InstallSectionProvider(runtime, *image); });
  }
  std::puts("section-provider-contracts-ok");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
