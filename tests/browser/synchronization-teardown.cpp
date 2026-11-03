#include "browser/synchronization-provider.h"
#include "browser/dispatcher-handles.h"
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

const auto entry = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  auto& memory = browser::Memory::From(base);
  const auto stack = ctx.esp;
  const auto create = [&](uint32_t output, uint32_t initial) {
    const std::array<uint32_t, 4> frame{0, output, 0, initial};
    ctx.esp -= sizeof(frame);
    memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    runtime.Kernel(ctx, base, 192);
    check(!ctx.eax && ctx.esp == stack, "teardown fixture mutant create failed");
  };
  const auto createEvent = [&](uint32_t output, uint32_t type, uint32_t initial) {
    const std::array<uint32_t, 5> frame{0, output, 0, type, initial};
    ctx.esp -= sizeof(frame);
    memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    runtime.Kernel(ctx, base, 189);
    check(!ctx.eax && ctx.esp == stack, "teardown fixture event create failed");
  };
  create(0x10100, 0);
  create(0x10104, 1);
  createEvent(0x10108, 1, 0);
  createEvent(0x1010C, 0, 1);
  ctx.esp += 8;
};

struct ProviderFixture {
  std::unique_ptr<browser::Runtime> runtime;
  std::unique_ptr<browser::PreparedImage> image;
  std::function<bool(uint32_t)> closer;
  uint32_t unowned = 0;
  uint32_t owned = 0;
  uint32_t syncEvent = 0;
  uint32_t notifyEvent = 0;
};

const auto makeProviderFixture = [] {
  ProviderFixture fixture;
  fixture.image = std::make_unique<browser::PreparedImage>();
  fixture.image->memory = std::make_unique<browser::Memory>();
  fixture.image->memory->Ensure(0x10000, 4096);
  fixture.image->image.kernel_thunk = 0x10000;
  fixture.image->image.entry = 0x11000;
  const std::array<X86FuncMapping, 1> mappings{{{0x11000, entry}}};
  fixture.runtime = std::make_unique<browser::Runtime>(mappings, std::span<const uint32_t>{});
  fixture.closer = browser::InstallSynchronizationProvider(*fixture.runtime, *fixture.image);
  browser::BindImageImports(*fixture.image, *fixture.runtime, {});
  browser::ImageExecution execution;
  browser::ExecuteImageEntry(*fixture.image, *fixture.runtime, execution);
  fixture.image->memory->Read({}, 0x10100, &fixture.unowned, 4);
  fixture.image->memory->Read({}, 0x10104, &fixture.owned, 4);
  fixture.image->memory->Read({}, 0x10108, &fixture.syncEvent, 4);
  fixture.image->memory->Read({}, 0x1010C, &fixture.notifyEvent, 4);
  check(fixture.unowned >= 0x1000 && fixture.owned >= 0x1000 && fixture.syncEvent >= 0x1000 &&
    fixture.notifyEvent >= 0x1000 && fixture.unowned != fixture.owned && fixture.unowned != fixture.syncEvent &&
    fixture.owned != fixture.syncEvent && fixture.syncEvent != fixture.notifyEvent, "teardown fixture handles invalid");
  check(fixture.closer(fixture.syncEvent) && !fixture.closer(fixture.syncEvent),
    "live provider closer did not retire the event exactly once");
  return fixture;
};

const auto testProviderRuntimeFirst = [] {
  auto fixture = makeProviderFixture();
  fixture.runtime.reset();
  fixture.image.reset();
  check(!fixture.closer(fixture.unowned) && !fixture.closer(fixture.owned) && !fixture.closer(fixture.notifyEvent),
    "retired provider closer claimed live handles");
};

const auto testProviderImageFirst = [] {
  auto fixture = makeProviderFixture();
  fixture.image.reset();
  check(!fixture.closer(fixture.unowned) && !fixture.closer(fixture.owned) && !fixture.closer(fixture.notifyEvent),
    "image-first retired provider closer claimed live handles");
  fixture.runtime.reset();
};

const auto testDispatcherTeardown = [](bool runtimeFirst, uint32_t owner) {
  auto runtime = std::make_unique<browser::Runtime>(std::span<const X86FuncMapping>{}, std::span<const uint32_t>{});
  auto dispatcher = std::make_unique<browser::Dispatcher>(*runtime);
  auto handles = std::make_unique<browser::DispatcherHandles>(*dispatcher);
  handles->CreateMutant(owner);
  handles->CreateEvent(false, false);
  handles->CreateTimer(false);
  dispatcher->Disconnect();
  handles.reset();
  if (runtimeFirst) {
    runtime.reset();
    dispatcher.reset();
  } else {
    dispatcher.reset();
    runtime.reset();
  }
};

const auto testCrossRuntimeGuards = [] {
  {
    browser::Runtime first({}, {}), second({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    browser::InstallSynchronizationProvider(first, image);
    bool refused = false;
    try { browser::BindImageImports(image, second, {}); } catch (const browser::Fault&) { refused = true; }
    check(refused && !image.importsBound, "install on runtime A then bind to runtime B was accepted");
  }
  {
    browser::Runtime first({}, {}), second({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    browser::InstallSynchronizationProvider(first, image);
    bool refused = false;
    try { browser::InstallSynchronizationProvider(second, image); } catch (const browser::Fault&) { refused = true; }
    check(refused, "synchronization install accepted a second runtime owner");
  }
  {
    browser::Runtime first({}, {}), second({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    browser::BindImageImports(image, first, {});
    bool refused = false;
    try { browser::InstallSynchronizationProvider(second, image); } catch (...) { refused = true; }
    check(refused, "bind on runtime A then synchronization install on runtime B was accepted");
  }
};

int main() try {
  testCrossRuntimeGuards();
  for (const bool runtimeFirst : {true, false}) {
    testDispatcherTeardown(runtimeFirst, 0);
    testDispatcherTeardown(runtimeFirst, 0x42);
  }
  testProviderImageFirst();
  testProviderRuntimeFirst();
  std::puts("synchronization-teardown-contracts-ok");
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
