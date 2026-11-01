#include "browser/platform-profile.h"
#include <array>
#include <cstdio>
#include <string_view>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, disableCrypto, (), {
  globalThis.profileSavedCrypto = globalThis.crypto;
  Object.defineProperty(globalThis, 'crypto', { configurable: true, value: undefined });
});
EM_JS(void, breakCrypto, (), {
  Object.defineProperty(globalThis, 'crypto', { configurable: true, value: {
    getRandomValues: () => { throw new Error('fixture entropy unavailable'); }
  }});
});
EM_JS(void, restoreCrypto, (), {
  Object.defineProperty(globalThis, 'crypto', { configurable: true, value: globalThis.profileSavedCrypto });
  delete globalThis.profileSavedCrypto;
});
#endif

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const std::array<uint8_t, 16> expected = {0x20, 0, 0, 0, 0xd3, 0xb1, 0, 0, 1, 0, 0, 0, 0xce, 0x16, 1, 0};
const auto entry = [](X86Context& context, uint8_t* base) {
  auto& memory = browser::Memory::From(base);
  std::array<uint32_t, 8> targets{};
  memory.Read(context, 0x10000, targets.data(), sizeof(targets));
  std::array<uint8_t, 16> bytes{};
  memory.Read(context, targets[0], bytes.data(), 8);
  memory.Read(context, targets[1], bytes.data() + 8, 8);
  check(bytes == expected, "guest received incorrect approved platform bytes");
  check(targets[2] == 0xc0000010, "guest launch cell target changed");
  uint32_t launchPage = UINT32_MAX;
  memory.Read(context, targets[2], &launchPage, 4);
  check(launchPage == 0, "guest did not observe a null cold boot launch page");
  launchPage = 0x12345000;
  memory.Write(context, targets[2], &launchPage, 4);
  memory.Read(context, targets[2], &launchPage, 4);
  check(launchPage == 0x12345000, "guest launch cell is not writable");
  std::array<uint16_t, 2> lengths{};
  uint32_t modelAddress = 0;
  memory.Read(context, targets[5], lengths.data(), 4);
  memory.Read(context, targets[5] + 4, &modelAddress, 4);
  std::array<char, 22> model{};
  memory.Read(context, modelAddress, model.data(), model.size());
  check(lengths[0] == 21 && lengths[1] == 22 && std::string_view(model.data()) == "Browser Virtual Cache",
    "guest received incorrect virtual model descriptor");
  memory.Read(context, targets[6], lengths.data(), 4);
  uint32_t serialAddress = 0;
  memory.Read(context, targets[6] + 4, &serialAddress, 4);
  std::array<char, 21> serial{};
  memory.Read(context, serialAddress, serial.data(), serial.size());
  check(lengths[0] == 20 && lengths[1] == 21 && serial.back() == 0, "guest serial descriptor changed");
  for (uint32_t n = 0; n < 20; ++n) {
    check((serial[n] >= '0' && serial[n] <= '9') || (serial[n] >= 'A' && serial[n] <= 'F'),
      "guest serial is not uppercase hexadecimal");
  }
  check(targets[7] == 0xc0000054, "virtual boot video pack target changed");
  uint32_t videoPack = 0;
  memory.Read(context, targets[7], &videoPack, 4);
  check(videoPack == 4, "guest did not observe diagnostic virtual HDTV pack metadata");
  context.esp += 8;
};

const auto refuse = [](const auto& operation) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, "invalid virtual drive key installation succeeded");
};

struct KeyFixture {
  browser::Runtime runtime{{}, {}};
  browser::PreparedImage image;
  KeyFixture() {
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 2> imports = {0x80000143, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 323}};
  }
  browser::KernelDataExport Install() { return browser::InstallVirtualDriveKey(runtime, image); }
  void RejectUnchanged(std::string_view reason = {}) {
    const auto backing = image.memory->BackingBytes();
    auto* owner = image.diagnosticRuntime;
    bool rejected = false;
    try { Install(); } catch (const browser::Fault& fault) {
      rejected = true;
      check(std::string_view(fault.what()).starts_with(reason), "key failure did not report its explicit cause");
    }
    check(rejected, "invalid virtual drive key installation succeeded");
    check(image.memory->BackingBytes() == backing && image.diagnosticRuntime == owner,
      "rejected key installation changed storage or ownership");
  }
};

const auto virtualKeys = [] {
  std::array<std::array<uint8_t, 16>, 3> keys{};
  for (uint32_t n = 0; n < keys.size(); ++n) {
    KeyFixture fixture;
    auto& memory = *fixture.image.memory;
    memory.Ensure(0xc0002000, 4096);
    memory.Ensure(0xc0004000, 4096);
    const uint32_t sentinel = 0xaabbccdd;
    memory.Write({}, 0xc0002ffc, &sentinel, 4);
    memory.Write({}, 0xc0004000, &sentinel, 4);
    const auto data = fixture.Install();
    check(data.ordinal == 323 && data.address == 0xc0003000 && data.size == 16,
      "virtual key export changed the sixteen byte ABI");
    const auto page = memory.QueryPages(data.address);
    check(page.mapped && page.committed && page.mappingBase == data.address && page.end == data.address + 4096,
      "virtual key is not stored in its dedicated owned page");
    memory.Read({}, data.address, keys[n].data(), keys[n].size());
    check(keys[n] != std::array<uint8_t, 16>{}, "virtual key used a zero fallback");
    for (const auto& priorKey : std::span(keys).first(n)) {
      check(keys[n] != priorKey, "fresh session reused a virtual key");
    }
    std::array<uint8_t, 4080> tail{};
    memory.Read({}, data.address + 16, tail.data(), tail.size());
    check(tail == std::array<uint8_t, 4080>{}, "entropy write exceeded the key span");
    for (const uint32_t address : {0xc0002ffcu, 0xc0004000u}) {
      uint32_t neighbor = 0;
      memory.Read({}, address, &neighbor, 4);
      check(neighbor == sentinel, "key installation touched a guard neighbor");
    }
    fixture.RejectUnchanged();
    browser::Runtime foreign({}, {});
    refuse([&] { browser::BindImageImports(fixture.image, foreign, std::span(&data, 1)); });
    check(!fixture.image.importsBound, "foreign runtime published a virtual key");
    browser::BindImageImports(fixture.image, fixture.runtime, std::span(&data, 1));
    check(fixture.image.boundImports[0].kind == browser::ImportKind::Data && !fixture.runtime.KernelTarget(323),
      "virtual key was not bound as data");
    const auto thread = browser::PrepareImageThread(fixture.image, fixture.runtime, 0x10, 0x10000);
    const auto& expectedKey = keys[n];
    fixture.runtime.Launch(thread.context, memory.Base(), [&](X86Context& context, uint8_t* base) {
      uint32_t target = 0;
      auto& guest = browser::Memory::From(base);
      guest.Read(context, 0x10000, &target, 4);
      for (uint32_t n = 0; n < 50; ++n) {
        std::array<uint8_t, 16> observed{};
        guest.Read(context, target, observed.data(), observed.size());
        check(observed == expectedKey, "session key changed during guest execution");
      }
    });
    if (auto failure = fixture.runtime.Join()) { std::rethrow_exception(failure); }
    fixture.RejectUnchanged();
    fixture.runtime.Stop();
    fixture.RejectUnchanged();
  }
};

const auto invalidProfiles = [] {
  const auto reject = [](browser::Runtime& runtime, browser::PreparedImage& image) {
    const auto backing = image.memory->BackingBytes();
    auto* owner = image.diagnosticRuntime;
    refuse([&] { browser::InstallPlatformProfile(runtime, image); });
    check(image.memory->BackingBytes() == backing && image.diagnosticRuntime == owner &&
      !image.memory->QueryPages(0xc0000000).mapped, "rejected profile changed storage or ownership");
  };
  for (const uint32_t ordinal : {41u, 42u, 164u, 322u, 324u, 356u}) {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    runtime.RegisterKernel(ordinal, [](X86Context&, uint8_t*) {});
    reject(runtime, image);
  }
  for (uint32_t n = 0; n < 8; ++n) {
    browser::Runtime runtime({}, {});
    browser::Runtime foreign({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    if (n == 0) { runtime.Cancel(); }
    if (n == 1) { runtime.PublishImageMemory(*image.memory, runtime.KernelRevision(), [] {}); }
    if (n == 2) { image.diagnosticRuntime = &foreign; }
    if (n == 3) {
      image.imports = {{0x10000, 41}};
      browser::DeclareDiagnosticImport(image, runtime, 41, true);
    }
    if (n == 4) { image.boundRuntime = &foreign; }
    if (n == 5) { image.importsBound = true; }
    if (n == 6 || n == 7) {
      image.imports = {{0x10000, 356}};
      browser::DeclareDiagnosticImport(image, runtime, 356, n == 6);
    }
    reject(runtime, image);
  }
  {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    auto lease = image.memory->Borrow();
    reject(runtime, image);
  }
  {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    runtime.Launch({}, image.memory->Base(), [&](X86Context&, uint8_t*) { reject(runtime, image); });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
  {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0xc0000000, 4096);
    const uint32_t sentinel = 0xaabbccdd;
    image.memory->Write({}, 0xc0000000, &sentinel, 4);
    const auto backing = image.memory->BackingBytes();
    refuse([&] { browser::InstallPlatformProfile(runtime, image); });
    uint32_t observed = 0;
    image.memory->Read({}, 0xc0000000, &observed, 4);
    check(observed == sentinel && image.memory->BackingBytes() == backing && !image.diagnosticRuntime,
      "occupied profile storage changed");
  }
};

const auto invalidKeys = [] {
  {
    KeyFixture fixture;
    fixture.runtime.RegisterKernel(323, [](X86Context&, uint8_t*) {});
    fixture.RejectUnchanged();
  }
  for (const bool data : {false, true}) {
    KeyFixture fixture;
    browser::DeclareDiagnosticImport(fixture.image, fixture.runtime, 323, data);
    fixture.RejectUnchanged();
  }
  {
    KeyFixture fixture;
    fixture.image.memory->Ensure(0xc0003000, 4096);
    const uint32_t sentinel = 0xaabbccdd;
    fixture.image.memory->Write({}, 0xc0003000, &sentinel, 4);
    fixture.RejectUnchanged();
    uint32_t observed = 0;
    fixture.image.memory->Read({}, 0xc0003000, &observed, 4);
    check(observed == sentinel, "occupied key storage was overwritten");
  }
  {
    KeyFixture fixture;
    fixture.runtime.Cancel();
    fixture.RejectUnchanged();
  }
  {
    KeyFixture fixture;
    fixture.runtime.PublishImageMemory(*fixture.image.memory, fixture.runtime.KernelRevision(), [] {});
    fixture.RejectUnchanged();
  }
  {
    KeyFixture fixture;
    browser::Memory foreign;
    fixture.runtime.PublishImageMemory(foreign, fixture.runtime.KernelRevision(), [] {});
    fixture.RejectUnchanged();
  }
  {
    KeyFixture fixture;
    browser::Runtime foreign({}, {});
    fixture.image.diagnosticRuntime = &foreign;
    fixture.RejectUnchanged();
  }
  {
    KeyFixture fixture;
    fixture.Install();
    auto original = std::move(fixture.image.memory);
    fixture.image.memory = std::make_unique<browser::Memory>();
    fixture.RejectUnchanged();
    fixture.image.memory = std::move(original);
    fixture.RejectUnchanged();
  }
  {
    KeyFixture fixture;
    fixture.runtime.Launch({}, fixture.image.memory->Base(), [&](X86Context&, uint8_t*) {
      fixture.RejectUnchanged();
    });
    if (auto failure = fixture.runtime.Join()) { std::rethrow_exception(failure); }
    check(!fixture.image.memory->QueryPages(0xc0003000).mapped, "active installation allocated key storage");
  }
  {
    KeyFixture fixture;
    auto lease = fixture.image.memory->Borrow();
    fixture.RejectUnchanged();
  }
  {
    KeyFixture fixture;
    fixture.image.memory.reset();
    refuse([&] { fixture.Install(); });
  }
#ifdef __EMSCRIPTEN__
  {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    disableCrypto();
    refuse([&] { browser::InstallPlatformProfile(runtime, image); });
    check(!image.memory->QueryPages(0xc0000000).mapped && !image.diagnosticRuntime,
      "missing entropy published a virtual serial");
    breakCrypto();
    refuse([&] { browser::InstallPlatformProfile(runtime, image); });
    check(!image.memory->QueryPages(0xc0000000).mapped && !image.diagnosticRuntime,
      "failed entropy published a virtual serial");
    restoreCrypto();
  }
  {
    KeyFixture fixture;
    disableCrypto();
    fixture.RejectUnchanged("secure virtual drive key entropy unavailable");
    check(!fixture.image.memory->QueryPages(0xc0003000).mapped && !fixture.image.importsBound,
      "missing WebCrypto published a key or allocated storage");
    breakCrypto();
    fixture.RejectUnchanged("secure virtual drive key entropy unavailable");
    check(!fixture.image.memory->QueryPages(0xc0003000).mapped,
      "failed WebCrypto allocated key storage");
    restoreCrypto();
    fixture.Install();
  }
#endif
};

int main() {
  virtualKeys();
  invalidKeys();
  invalidProfiles();
  {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    const auto backing = image.memory->BackingBytes();
    runtime.RegisterKernel(164, [](X86Context&, uint8_t*) {});
    bool rejected = false;
    try { browser::InstallPlatformProfile(runtime, image); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "launch data export accepted a function target");
    check(image.memory->BackingBytes() == backing, "rejected platform installation allocated storage");
  }
  std::array<std::array<char, 21>, 3> serials{};
  for (uint32_t n = 0; n < 3; ++n) {
    const std::array<X86FuncMapping, 1> mappings = {{{0x11000, entry}}};
    browser::Runtime runtime(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    image.image.entry = 0x11000;
    const std::array<uint32_t, 9> imports = {0x80000142, 0x80000144, 0x800000a4, 0x80000143, 0x8000015d, 0x80000029, 0x8000002a, 0x80000164, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 322}, {0x10004, 324}, {0x10008, 164}, {0x1000c, 323}, {0x10010, 349}, {0x10014, 41}, {0x10018, 42}, {0x1001c, 356}};
    const auto key = browser::InstallVirtualDriveKey(runtime, image);
    const auto backing = image.memory->BackingBytes();
    const auto exports = browser::InstallPlatformProfile(runtime, image);
    check(exports.size() == 6, "virtual boot video pack data export is absent");
    check(exports[5].ordinal == 356 && exports[5].address == 0xc0000054 && exports[5].size == 4 &&
      !runtime.KernelTarget(356), "virtual boot video pack export is not an exact data word");
    check(image.memory->BackingBytes() == backing + 4097, "virtual boot pack allocated a separate platform page");
    uint32_t videoPack = 0;
    image.memory->Read({}, exports[5].address, &videoPack, sizeof(videoPack));
    check(videoPack == 4, "virtual boot video pack does not match diagnostic HDTV profile");
    std::array<uint8_t, 4015> tail{}, expectedTail{};
    expectedTail[3] = 4;
    image.memory->Read({}, 0xc0000051, tail.data(), tail.size());
    check(tail == expectedTail, "virtual boot pack exceeded its exact output span or serial padding");
    check(exports[4].ordinal == 42 && exports[4].address == 0xc0000034 && exports[4].size == 8,
      "virtual serial descriptor ABI changed");
    std::array<uint16_t, 2> serialLengths{};
    uint32_t serialAddress = 0;
    image.memory->Read({}, exports[4].address, serialLengths.data(), 4);
    image.memory->Read({}, exports[4].address + 4, &serialAddress, 4);
    check(serialLengths[0] == 20 && serialLengths[1] == 21 && serialAddress == 0xc000003c,
      "virtual serial buffer bounds or lengths changed");
    image.memory->Read({}, serialAddress, serials[n].data(), serials[n].size());
    check(serials[n].back() == 0, "virtual serial is not terminated");
    for (uint32_t digit = 0; digit < 20; ++digit) {
      const char value = serials[n][digit];
      check((value >= '0' && value <= '9') || (value >= 'A' && value <= 'F'), "invalid virtual serial digit");
    }
    for (const auto& prior : std::span(serials).first(n)) {
      check(serials[n] != prior, "fresh session reused a virtual serial");
    }
    check(image.diagnosticRuntime == &runtime && key.ordinal == 323, "key before profile lost ownership");
    check(exports[3].ordinal == 41 && exports[3].size == 8 && exports[3].address >= 0xc0000014 &&
      exports[3].address + 8 <= 0xc0001000, "virtual model descriptor is outside owned profile page");
    std::array<uint16_t, 2> lengths{};
    uint32_t modelAddress = 0;
    image.memory->Read({}, exports[3].address, lengths.data(), 4);
    image.memory->Read({}, exports[3].address + 4, &modelAddress, 4);
    check(lengths[0] == 21 && lengths[1] == 22 && modelAddress >= 0xc0000014 &&
      modelAddress + lengths[1] <= 0xc0001000, "virtual model buffer bounds or lengths changed");
    std::array<char, 22> model{};
    image.memory->Read({}, modelAddress, model.data(), model.size());
    check(model.back() == 0 && std::string_view(model.data()) == "Browser Virtual Cache",
      "virtual model label changed across sessions");
    check(exports[0].ordinal == 322 && exports[1].ordinal == 324 && exports[0].size == 8 && exports[1].size == 8 &&
      exports[0].address == 0xc0000000 && exports[1].address == 0xc0000008,
      "platform export ABI changed");
    check(exports[2].ordinal == 164 && exports[2].address == 0xc0000010 && exports[2].size == 4,
      "launch data pointer cell ABI changed");
    uint32_t launchPage = UINT32_MAX;
    image.memory->Read({}, exports[2].address, &launchPage, 4);
    check(launchPage == 0, "fresh session inherited a launch page");
    launchPage = 0x56789000;
    image.memory->Write({}, exports[2].address, &launchPage, 4);
#ifndef __EMSCRIPTEN__
    {
      browser::Runtime isolatedRuntime({}, {});
      browser::PreparedImage isolated;
      isolated.memory = std::make_unique<browser::Memory>();
      const auto isolatedExports = browser::InstallPlatformProfile(isolatedRuntime, isolated);
      isolated.memory->Read({}, isolatedExports[2].address, &launchPage, 4);
      check(launchPage == 0, "simultaneous session inherited a launch page");
      uint32_t isolatedPack = 0;
      isolated.memory->Read({}, isolatedExports[5].address, &isolatedPack, 4);
      check(isolatedPack == 4, "isolated virtual boot pack profile diverged");
      image.memory->Read({}, exports[2].address, &launchPage, 4);
      check(launchPage == 0x56789000, "isolated installation reset another session's launch cell");
    }
#endif
    bool duplicate = false;
    try { browser::InstallPlatformProfile(runtime, image); }
    catch (const std::runtime_error&) { duplicate = true; }
    check(duplicate, "platform storage was silently replaced");
    image.memory->Read({}, exports[2].address, &launchPage, 4);
    check(launchPage == 0x56789000, "duplicate installation reset writable launch data");
    launchPage = 0;
    image.memory->Write({}, exports[2].address, &launchPage, 4);
    browser::DeclareDiagnosticImport(image, runtime, 323, true);
    browser::DeclareDiagnosticImport(image, runtime, 349, false);
    browser::Runtime foreign({}, {});
    bool foreignRejected = false;
    try { browser::BindImageImports(image, foreign, exports); }
    catch (const std::runtime_error&) { foreignRejected = true; }
    check(foreignRejected && !image.importsBound, "foreign runtime bound platform session");
    for (const uint32_t address : {0u, 0xc0000ffeu, 0xc0001000u, 0x10000u, UINT32_MAX}) {
      auto invalid = exports;
      invalid[5].address = address;
      const auto bytesBefore = image.memory->BackingBytes();
      refuse([&] { browser::BindImageImports(image, runtime, invalid); });
      std::array<uint32_t, 9> slots{};
      image.memory->Read({}, 0x10000, slots.data(), sizeof(slots));
      check(slots == imports && !image.importsBound && image.boundImports.empty() &&
        image.diagnosticRuntime == &runtime && image.memory->BackingBytes() == bytesBefore,
        "invalid virtual boot pack binding partially published");
    }
    browser::BindImageImports(image, runtime, exports);
    check(image.boundImports[7].ordinal == 356 && image.boundImports[7].kind == browser::ImportKind::Data &&
      image.boundImports[7].target == exports[5].address && !runtime.KernelTarget(356),
      "actual ordinal 356 did not bind as virtual pack data");
    refuse([&] { runtime.RegisterKernel(356, [](X86Context&, uint8_t*) {}); });
    bool rebound = false;
    try { browser::BindImageImports(image, runtime, exports); }
    catch (const std::runtime_error&) { rebound = true; }
    check(rebound, "published virtual boot pack imports remained replaceable");
    uint32_t boundPack = 0;
    image.memory->Read({}, 0x1001c, &boundPack, 4);
    check(boundPack == exports[5].address, "published virtual boot pack target changed");
    check(image.boundImports[0].kind == browser::ImportKind::Data && image.boundImports[1].kind == browser::ImportKind::Data &&
      image.boundImports[2].kind == browser::ImportKind::Data && !runtime.KernelTarget(164),
      "approved data remained guarded or acquired a function");
    check(image.boundImports[3].kind == browser::ImportKind::DataGuard && image.boundImports[4].kind == browser::ImportKind::FunctionTrap,
      "unapproved exports lost their traps");
    bool sealed = false;
    try { browser::InstallPlatformProfile(runtime, image); }
    catch (const std::runtime_error&) { sealed = true; }
    check(sealed, "published platform policy remained replaceable");
    browser::ImageExecution execution;
    browser::ExecuteImageEntry(image, runtime, execution);
    check(execution.entered && execution.returned && !runtime.ActiveWorkers(), "platform entry did not complete and join");
    std::array<char, 21> observedSerial{};
    image.memory->Read({}, serialAddress, observedSerial.data(), observedSerial.size());
    check(observedSerial == serials[n], "virtual serial changed during session");
    image.memory->Read({}, exports[5].address, &videoPack, sizeof(videoPack));
    check(videoPack == 4, "virtual boot pack metadata changed during session");
    bool guarded = false;
    uint8_t value = 0;
    try { image.memory->Read({}, image.boundImports[3].target, &value, 1); }
    catch (const browser::Fault& fault) { guarded = browser::DiagnosticDataOrdinal(image, fault.address) == 323; }
    check(guarded, "unapproved key data became accessible");
    bool trapped = false;
    auto context = execution.context;
    try { runtime.Invoke(context, image.memory->Base(), runtime.KernelTarget(349)); }
    catch (const browser::UnsupportedImport& fault) { trapped = fault.ordinal == 349; }
    check(trapped, "unapproved function stopped trapping");
  }
  std::puts("platform-profile-contracts-ok");
}
