#include "browser/diagnostic-graphics.h"
#include "browser/sdk-provider.h"
#include "cpu/x86_ops.h"
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto rejects = [](const auto& action) {
  bool refused = false;
  try { action(); } catch (const browser::Fault&) { refused = true; }
  check(refused, "unavailable graphics operation accepted");
};
X86_FUNC(__imp__sub_000534B8) { throw browser::Fault(ctx, 0x534b8, 0, "signature Begin is absent from graphics fixture"); }
X86_FUNC(__imp__sub_0005351A) { throw browser::Fault(ctx, 0x5351a, 0, "signature End is absent from graphics fixture"); }
std::atomic<uint32_t> defaults = 0;
std::atomic<uint32_t> originals = 0;
std::atomic<uint32_t> defaultMode = 0;
X86_FUNC(sub_00F11000);
X86_FUNC(sub_00F12000);
X86_FUNC(sub_00F13000);
X86_FUNC(__imp__sub_00F13000);
X86_FUNC(sub_0049CE00);
X86_FUNC(sub_0049DE60);
X86_FUNC(sub_004A17C0);
X86_FUNC(sub_0049BD00);
X86_FUNC(sub_0049BF30);
X86_FUNC(sub_0049BD80);
X86_FUNC(__imp__sub_0049BD80);
std::vector<uint32_t> destroyedResources;
bool destroyFails = false;
void DestroyResource(X86Context& ctx, uint8_t* base) {
  check(!browser::Memory::HasBorrow(), "resource destruction borrowed guest memory");
  if (destroyFails) { throw browser::Fault(ctx, 0, 0, "injected destruction failure"); }
  auto& memory = browser::Memory::From(base);
  uint32_t header;
  memory.Read(ctx, ctx.esp + 4, &header, 4);
  destroyedResources.push_back(header);
  memory.Decommit(header, 0x1000);
  ctx.eax = 0x12345678;
  ctx.ecx ^= 0xabcdef;
  ctx.esp += 8;
}
X86_FUNC(sub_0049CC50);
X86_FUNC(sub_0049A3D0);
X86_FUNC(sub_00F14000);
X86_FUNC(__imp__sub_00F14000);
X86_FUNC(sub_0049EFF0);
X86_FUNC(sub_0049EFC0);
X86_FUNC(__imp__sub_0049EFC0) { throw std::runtime_error("original fastcall state setter should not be called"); }
X86_FUNC(sub_0049E8B0);
X86_FUNC(__imp__sub_0049EFF0) { throw std::runtime_error("original state setter should not be called"); }
X86_FUNC(__imp__sub_0049E8B0) { throw std::runtime_error("unimplemented constant setter should not be called"); }
X86_FUNC(sub_004A61A0);
X86_FUNC(sub_004A6240);
X86_FUNC(__imp__sub_0049A3D0) { throw std::runtime_error("original Gamma should not be called"); }
X86_FUNC(__imp__sub_004A61A0) { throw std::runtime_error("original Draw should not be called"); }
X86_FUNC(__imp__sub_004A6240) { throw std::runtime_error("original DrawIndexed should not be called"); }
X86_FUNC(sub_004A1A80) {
  check(!browser::Memory::HasBorrow(), "retained defaults ran under lease");
  ++defaults;
  auto& memory = browser::Memory::From(base);
  uint32_t push;
  memory.Read(ctx, 0x4ab670, &push, 4);
  const std::array<uint32_t, 2> commands = {0x00040300, 1};
  memory.Write(ctx, push, commands.data(), sizeof(commands));
  push += sizeof(commands);
  memory.Write(ctx, 0x4ab670, &push, 4);
  const uint32_t cpuState = 0x11223344;
  memory.Write(ctx, 0x4ab670 + 0xc60, &cpuState, 4);
  if (defaultMode == 1) { throw browser::Fault(ctx, 0x777, 4, "retained-default-rejection"); }
  if (defaultMode == 2) {
    auto& runtime = *static_cast<browser::Runtime*>(ctx.host);
    runtime.Cancel();
    runtime.Checkpoint(ctx);
  }
  ctx.esp += 4;
}

int checkpointContracts() {
  browser::PreparedImage image;
  browser::Runtime runtime(std::array<X86FuncMapping, 3>{{{0xf11000, sub_00F11000}, {0xf12000, sub_00F12000}, {0xf13000, sub_00F13000}}}, {});
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 0x1000);
  image.memory->Ensure(0x20000, 0x4000);
  image.image.kernel_thunk = 0x10000;
  browser::ContiguousMemoryAccess access;
  browser::QueryContiguousMemoryUsage usage;
  browser::InstallContiguousMemory(runtime, *image.memory, &usage, &access);
  auto graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, image, access, browser::GraphicsMode::Checkpoint);
  rejects([&] { graphics->ConfigurePresentCheckpoint(1); });
  graphics->ConfigurePresentCheckpoint(0);
  {
    auto lease = image.memory->Borrow();
    rejects([&] { graphics->ConfigurePresentCheckpoint(0); });
  }
  browser::InstallSdkProvider(runtime, image, browser::SdkPolicy::UnavailableXNet, graphics);
  browser::BindImageImports(image, runtime, {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x22000;
  context.fs_base = 0x12340000;
  const std::array<uint32_t, 7> frame = {0xabcdef, 0, 1, 0, 0x40, 0x20000, 0x20100};
  const std::array<uint32_t, 13> presentation = {640, 480, 18, 1};
  const uint32_t sentinel = 0xdeadbeef;
  image.memory->Write(context, context.esp, frame.data(), sizeof(frame));
  image.memory->Write(context, frame[5], presentation.data(), sizeof(presentation));
  image.memory->Write(context, frame[6], &sentinel, sizeof(sentinel));
  bool checkpoint = false;
  runtime.Launch(context, image.memory->Base(), [&](X86Context& current, uint8_t* base) {
    const auto before = current;
    try { sub_00F13000(current, base); }
    catch (const browser::GraphicsCheckpoint& fault) {
      checkpoint = true;
      check(fault.context->esp == before.esp && fault.context->fs_base == before.fs_base, "graphics checkpoint lost its genuine context");
    }
    uint32_t output;
    image.memory->Read(current, frame[6], &output, sizeof(output));
    check(checkpoint && output == sentinel && usage(current, *image.memory) == 0 &&
      !std::memcmp(&current, &before, sizeof(current)), "graphics checkpoint mutated device outputs, allocations or call state");
  });
  check(!runtime.Join() && checkpoint && runtime.ActiveWorkers() == 0, "diagnostic checkpoint did not join");
  const auto snapshot = graphics->Snapshot();
  check(!snapshot.initialized && snapshot.count == 1 && snapshot.events[0].request == browser::DiagnosticGraphics::Request::Init &&
    snapshot.events[0].arguments[0] == 640 && snapshot.events[0].arguments[1] == 480 && snapshot.resources.empty(),
    "graphics checkpoint falsely initialized a device or lost its original request");
  return 0;
}

class ProbeInput final : public browser::SdkGraphics {
  browser::Runtime& runtime_;
  browser::PreparedImage& image_;
public:
  uint32_t calls = 0;
  uint32_t expected = 43;
  ProbeInput(browser::Runtime& runtime, browser::PreparedImage& image) : runtime_(runtime), image_(image) {}
  bool BelongsTo(browser::Runtime& runtime, browser::PreparedImage& image) const override { return &runtime == &runtime_ && &image == &image_; }
  bool Dispatch(X86Context&, uint8_t*, uint32_t id) override {
    check(id == expected, "SDK service received an unrelated operation");
    ++calls;
    return false;
  }
};

class ProbeBackend final : public gpu::Backend {
public:
  std::shared_ptr<uint8_t> lifetime = std::make_shared<uint8_t>(0);
  uint32_t initializations = 0;
  uint32_t clears = 0;
  uint32_t draws = 0;
  uint32_t flags = 0;
  uint32_t lateSyncs = 0;
  uint32_t syncs = 0;
  bool rejectSync = false;
  std::function<void(uint32_t, uint32_t)> onSync;
  uint32_t presents = 0;
  uint32_t flushes = 0;
  bool rejectFlush = false;
  bool rejectPresent = false;
  gpu::Surface front;
  std::array<uint8_t, 768> gamma{};
  uint32_t gammaCalls = 0;
  std::vector<uint16_t> indices;
  std::vector<std::array<uint32_t, 2>> released;
  bool ReleaseResource(uint32_t header, uint32_t data) override {
    check(!browser::Memory::HasBorrow(), "resource retirement borrowed guest memory");
    check(std::find(destroyedResources.begin(), destroyedResources.end(), header) != destroyedResources.end(), "resource retired before genuine destruction");
    released.push_back({header, data});
    return true;
  }
  gpu::State state;
  bool Init(uint32_t width, uint32_t height) override {
    ++initializations;
    return width == 640 && height == 480;
  }
  void Clear(const gpu::State& value, uint32_t count, const uint8_t* rects, uint32_t mask, uint32_t, float, uint32_t) override {
    check(!count && !rects, "unexpected clear rectangles");
    state = value;
    flags = mask;
    ++clears;
  }
  void Draw(const gpu::State& value, uint32_t primitive, uint32_t start, uint32_t count, const uint8_t* data) override {
    check(primitive == 5 && !start && count == 3, "draw ABI mismatch");
    state = value;
    indices.resize(data ? count : 0);
    if (data) { std::memcpy(indices.data(), data, count * 2); }
    ++draws;
  }
  void Copy(const gpu::Surface&, const gpu::Surface&, uint32_t, uint32_t, uint32_t) override {}
  void Present(const gpu::Surface& value, bool pillarbox) override {
    check(!pillarbox, "unexpected diagnostic pillarbox substitution");
    if (rejectPresent) { throw std::runtime_error("injected present failure"); }
    front = value;
    ++presents;
  }
  void Sync(uint32_t address, uint32_t bytes) override {
    check(address >= 0x80000000 && (!bytes || bytes == 640 * 480 * 4), "resource sync ABI mismatch");
    if (rejectSync) { throw std::runtime_error("injected sync failure"); }
    if (onSync) { onSync(address, bytes); }
    ++syncs;
  }
  void SyncLate(uint32_t address) override {
    check(address >= 0x80000000, "SyncLate did not receive checked virtual resource data");
    ++lateSyncs;
  }
  void Flush() override {
    if (rejectFlush) { throw std::runtime_error("injected flush failure"); }
    ++flushes;
  }
  void SetGamma(const uint8_t* ramp) override {
    std::memcpy(gamma.data(), ramp, gamma.size());
    ++gammaCalls;
  }
};

int presentCheckpointContracts(uint32_t limit, bool backendFailure = false) {
  browser::PreparedImage image;
  browser::Runtime runtime(std::array<X86FuncMapping, 4>{{{0xf11000, sub_00F11000}, {0xf12000, sub_00F12000}, {0xf13000, sub_00F13000}, {0x4a1a80, sub_004A1A80}}}, {});
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 0x1000);
  image.memory->Ensure(0x20000, 0x4000);
  image.memory->Ensure(0x4ab000, 0x4000);
  image.image.kernel_thunk = 0x10000;
  browser::ContiguousMemoryAccess access;
  browser::InstallContiguousMemory(runtime, *image.memory, nullptr, &access);
  auto backend = std::make_unique<ProbeBackend>();
  auto* probe = backend.get();
  const std::weak_ptr<uint8_t> lifetime = backend->lifetime;
  auto graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, image, access, browser::GraphicsMode::Rendering, std::move(backend));
  rejects([&] { graphics->ConfigurePresentCheckpoint(10001); });
  rejects([&] { graphics->ConfigurePresentCheckpoint(UINT32_MAX); });
  graphics->ConfigurePresentCheckpoint(10000);
  graphics->ConfigurePresentCheckpoint(limit);
  const auto sdk = browser::InstallSdkProvider(runtime, image, browser::SdkPolicy::Observer, graphics);
  browser::BindImageImports(image, runtime, {});
  rejects([&] { graphics->ConfigurePresentCheckpoint(0); });
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x22000;
  const std::array<uint32_t, 7> frame{0xabcdef, 0, 1, 0, 0x50, 0x20000, 0x20100};
  const std::array<uint32_t, 13> presentation{640, 480, 18, 1, 0, 1, 0, 0, 1, 46, 320, 0, 0x80000002};
  image.memory->Write(context, context.esp, frame.data(), sizeof(frame));
  image.memory->Write(context, frame[5], presentation.data(), sizeof(presentation));
  std::atomic<bool> waiting = false;
  std::atomic<bool> cancelled = false;
  std::atomic<bool> returned = false;
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    sub_00F13000(ctx, base);
    check(probe->presents == 0 && graphics->Snapshot().completedPresents == 0, "checkpoint synthesized a Present during initialization");
    rejects([&] { graphics->ConfigurePresentCheckpoint(0); });
    if (limit) {
      auto sibling = ctx;
      sibling.esp = 0x23000;
      runtime.Launch(sibling, base, [&](X86Context& current, uint8_t*) {
        waiting = true;
        try {
          while (true) { runtime.Checkpoint(current); std::this_thread::yield(); }
        } catch (const browser::Cancelled&) { cancelled = true; throw; }
      });
      while (!waiting.load()) { std::this_thread::yield(); }
    }
    const std::array<uint32_t, 2> swap{0xabcdef, 0};
    const uint32_t attempts = limit ? limit : 3;
    for (uint32_t n = 0; n < attempts; ++n) {
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, swap.data(), sizeof(swap));
      probe->rejectPresent = backendFailure && n + 1 == attempts;
      sub_0049CC50(ctx, base);
      check(ctx.eax == n + 1 && probe->presents == n + 1 && graphics->Snapshot().completedPresents == n + 1,
        "successful Present was not published exactly once");
    }
    returned = true;
  });
  const auto failure = runtime.Join();
  check(runtime.ActiveWorkers() == 0, "Present checkpoint left guest workers alive");
  const uint32_t completed = limit ? limit - uint32_t(backendFailure) : 3;
  check(probe->presents == completed && graphics->Snapshot().completedPresents == completed,
    "failed backend Present was counted or checkpoint stopped at the wrong frame");
  uint32_t published;
  image.memory->Read({}, 0x4ab670 + 0x2478, &published, 4);
  check(published == completed, "terminal snapshot lost genuine guest swap count");
  check(sdk->Snapshot().count > 0, "terminal checkpoint lost real SDK observations");
  if (limit) {
    check(failure && cancelled && !returned, "checkpoint did not cancel and join its sibling worker");
    bool identified = false;
    try { std::rethrow_exception(failure); }
    catch (const browser::GraphicsPresentCheckpoint& fault) {
      check(!backendFailure && fault.completedPresents == limit && fault.context->eax == limit && fault.context->esp == 0x22008,
        "named Present checkpoint lost published count or completed Swap context");
      identified = true;
    } catch (const browser::Fault& fault) {
      check(backendFailure && std::string(fault.what()).starts_with("injected present failure") && fault.context->esp == 0x22000,
        "diagnostic checkpoint swallowed the original backend failure");
      identified = true;
    }
    check(identified, "terminal Present fault was not explicit");
  } else {
    check(!failure && returned && !cancelled, "disabled Present checkpoint changed normal execution");
  }
  graphics->Shutdown();
  check(lifetime.expired() && runtime.ActiveWorkers() == 0, "terminal checkpoint retained backend or workers after owned cleanup");
  rejects([&] { graphics->ConfigurePresentCheckpoint(0); });
  defaults = 0;
  return 0;
}

int captureContracts(bool rendering, bool failInitialization = false) {
  browser::PreparedImage image;
  browser::Runtime runtime(std::array<X86FuncMapping, 6>{{{0xf11000, sub_00F11000}, {0xf12000, sub_00F12000}, {0xf13000, sub_00F13000}, {0x4a1a80, sub_004A1A80}, {0x49bd00, sub_0049BD00}, {0x49bf30, sub_0049BF30}}}, {});
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 0x1000);
  image.memory->Ensure(0x20000, 0x4000);
  image.memory->Ensure(0x4ab000, 0x4000);
  image.memory->Ensure(0x4a8000, 0x1000);
  image.image.kernel_thunk = 0x10000;
  browser::ContiguousMemoryAccess access;
  browser::InstallContiguousMemory(runtime, *image.memory, nullptr, &access);
  auto backend = rendering ? std::make_unique<ProbeBackend>() : nullptr;
  auto* probe = backend.get();
  const auto backendLifetime = backend ? std::weak_ptr<uint8_t>(backend->lifetime) : std::weak_ptr<uint8_t>();
  auto graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, image, access,
    rendering ? browser::GraphicsMode::Rendering : browser::GraphicsMode::StateCapture, std::move(backend));
  auto input = std::make_shared<ProbeInput>(runtime, image);
  auto launch = std::make_shared<ProbeInput>(runtime, image);
  launch->expected = 64;
  auto route = std::make_shared<ProbeInput>(runtime, image);
  route->expected = 65;
  browser::InstallSdkProvider(runtime, image, browser::SdkPolicy::Observer, graphics, input, launch, route);
  browser::BindImageImports(image, runtime, {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x22000;
  const std::array<uint32_t, 7> frame{0xabcdef, 0, 1, 0, 0x50, 0x20000, 0x20100};
  const std::array<uint32_t, 13> presentation{640, 480, 18, 1, 0, 1, 0, 0, 1, 46, 320, 0, 0x80000002};
  image.memory->Write(context, context.esp, frame.data(), sizeof(frame));
  image.memory->Write(context, frame[5], presentation.data(), sizeof(presentation));
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    check(!browser::Runtime::ObserveSdkEntry(ctx, base, 43) && input->calls == 1, "SDK input service was not isolated from graphics");
    check(!browser::Runtime::ObserveSdkEntry(ctx, base, 64) && launch->calls == 1, "SDK launch service was not isolated from graphics");
    check(!browser::Runtime::ObserveSdkEntry(ctx, base, 65) && route->calls == 1, "SDK route service was not isolated from graphics");
    if (failInitialization) {
      defaultMode = 1;
      rejects([&] { sub_00F13000(ctx, base); });
      defaultMode = 0;
      rejects([&] { sub_00F13000(ctx, base); });
      check(probe->initializations == 1 && !graphics->Snapshot().initialized && graphics->Snapshot().resources.empty(),
        "failed genuine renderer initialization was retried as a fresh backend");
      return;
    }
    sub_00F13000(ctx, base);
    const auto snapshot = graphics->Snapshot();
    check(snapshot.initialized && snapshot.resources.size() == 3 && snapshot.commandWords == 2, "normal state capture omitted retained defaults or depth");
    const std::array<uint32_t, 2> disabledOverlay{0xabcdef, 0};
    ctx.esp = 0x22000;
    image.memory->Write(ctx, ctx.esp, disabledOverlay.data(), sizeof(disabledOverlay));
    bool retainedOverlayFault = false;
    try { __imp__sub_00F14000(ctx, base); } catch (const browser::Fault& fault) {
      retainedOverlayFault = fault.context->eax == 0 && fault.address < 0x10000 && !image.memory->IsCommitted(fault.address);
    }
    check(retainedOverlayFault, "retained overlay fixture did not expose absent hardware state");
    for (uint32_t n = 0; n < 2; ++n) {
      ctx.esp = 0x22000;
      ctx.eax = 0xdeadbeef;
      const auto before = ctx;
      const auto events = graphics->Snapshot().total;
      sub_00F14000(ctx, base);
      auto expected = before;
      expected.eax = 0;
      expected.esp += 8;
      check(!std::memcmp(&ctx, &expected, sizeof(ctx)), "disabled overlay changed native stdcall ABI or unrelated registers");
      check(graphics->Snapshot().total == events, "already disabled overlay submitted fake renderer work");
    }
    for (const uint32_t enabled : {1u, UINT32_MAX}) {
      const std::array<uint32_t, 2> overlay{0xabcdef, enabled};
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, overlay.data(), sizeof(overlay));
      const auto before = ctx;
      rejects([&] { sub_00F14000(ctx, base); });
      check(!std::memcmp(&ctx, &before, sizeof(ctx)), "unsupported overlay enable completed or changed caller state");
    }
    for (const uint32_t stack : {0x23ffcu, 0xfffffffcu}) {
      ctx.esp = stack;
      const auto before = ctx;
      rejects([&] { sub_00F14000(ctx, base); });
      check(!std::memcmp(&ctx, &before, sizeof(ctx)), "invalid overlay frame changed caller state");
    }
    const uint32_t encoding = gpu::kMBlendEnable;
    image.memory->Write(ctx, 0x4a8000 + 57 * 4, &encoding, 4);
    const std::array<uint32_t, 3> simple{0xabcdef, 57, 1};
    ctx.esp = 0x22000;
    image.memory->Write(ctx, ctx.esp, simple.data(), sizeof(simple));
    sub_0049EFF0(ctx, base);
    check(ctx.esp == 0x2200c, "exported simple state setter ABI incorrect");
    const std::array<uint32_t, 3> complex{0xabcdef, gpu::kRsCullMode, 0x900};
    ctx.esp = 0x22000;
    image.memory->Write(ctx, ctx.esp, complex.data(), sizeof(complex));
    sub_0049EFF0(ctx, base);
    check(ctx.esp == 0x2200c, "exported complex state setter ABI incorrect");
    const auto state = graphics->State();
    check(state.simple[gpu::kMBlendEnable >> 2] == 1 && state.rs[gpu::kRsCullMode] == 0x900, "native SDK state semantics were not captured");
    check(state.zs.header == snapshot.resources[2].header && ((state.zs.format >> 8) & 255) == 46, "state capture depth mismatch");
    if (rendering) {
      ctx.esp = 0x22000;
      const std::array<uint32_t, 1> idle{0xabcdef};
      image.memory->Write(ctx, ctx.esp, idle.data(), sizeof(idle));
      probe->rejectFlush = true;
      rejects([&] { graphics->Dispatch(ctx, base, 7); });
      check(ctx.esp == 0x22000 && probe->flushes == 0, "failed renderer drain completed idle wait");
      probe->rejectFlush = false;
      check(!graphics->Dispatch(ctx, base, 7) && ctx.esp == 0x22004 && ctx.eax == 0 && probe->flushes == 1,
        "idle wait did not drain through rendering backend");
      std::array<uint8_t, 768> ramp;
      for (uint32_t n = 0; n < ramp.size(); ++n) { ramp[n] = uint8_t(n); }
      image.memory->Write(ctx, 0x20800, ramp.data(), ramp.size());
      const std::array<uint32_t, 3> gamma{0xabcdef, 0, 0x20800};
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, gamma.data(), sizeof(gamma));
      sub_0049A3D0(ctx, base);
      check(ctx.esp == 0x2200c && ctx.eax == 0 && probe->gammaCalls == 1 && probe->gamma == ramp,
        "exported Gamma wrapper did not delegate checked native ramp ABI");
      auto invalid = gamma;
      invalid[2] = 0x23f00;
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, invalid.data(), sizeof(invalid));
      rejects([&] { sub_0049A3D0(ctx, base); });
      check(ctx.esp == 0x22000 && probe->gammaCalls == 1, "invalid Gamma source reached backend or advanced stack");
    }
    const auto typedState = std::make_unique<gpu::State>();
    const auto refresh = [&]() -> const gpu::State& {
      *typedState = graphics->State();
      return *typedState;
    };
    const auto typed = [&](uint32_t id, std::initializer_list<uint32_t> arguments) {
      std::vector<uint32_t> call{0xabcdef};
      call.insert(call.end(), arguments.begin(), arguments.end());
      ctx.esp = 0x22000;
      ctx.eax = 0xdeadbeef;
      image.memory->Write(ctx, ctx.esp, call.data(), call.size() * 4);
      check(!graphics->Dispatch(ctx, base, id), "typed state operation delegated to original");
      check(ctx.esp == 0x22000 + call.size() * 4 && ctx.eax == 0, "typed state operation cleanup mismatch");
    };
    ctx.esp = 0x22000;
    ctx.ecx = 0xa0040000 | gpu::kMSrcBlend | 3;
    ctx.edx = 0x302;
    sub_0049EFC0(ctx, base);
    check(ctx.esp == 0x22004 && ctx.eax == 0 && refresh().simple[gpu::kMSrcBlend >> 2] == 0x302,
      "fastcall simple state method was not masked and captured");
    const std::array<std::array<float, 4>, 4> constants{{{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}}};
    image.memory->Write(ctx, 0x20300, constants.data(), sizeof(constants));
    ctx.esp = 0x22000;
    ctx.ecx = 17;
    ctx.edx = 0x20300;
    sub_0049E8B0(ctx, base);
    check(ctx.esp == 0x22004 && refresh().vsc[17] == constants[0], "fastcall constants were not captured");
    ctx.ecx = 190;
    typed(31, {19});
    check(refresh().vsc[190] == constants[0] && refresh().vsc[191] == constants[1], "constant upload was not clipped to native register range");
    ctx.ecx = 192;
    ctx.edx = 0xffffffff;
    typed(30, {});
    ctx.ecx = 18;
    ctx.edx = 0x23ff0;
    const auto beforeConstants = refresh().vsc;
    ctx.esp = 0x22000;
    const std::array<uint32_t, 2> invalidConstants{0xabcdef, 8};
    image.memory->Write(ctx, ctx.esp, invalidConstants.data(), sizeof(invalidConstants));
    rejects([&] { graphics->Dispatch(ctx, base, 31); });
    check(ctx.esp == 0x22000 && refresh().vsc == beforeConstants, "partially readable constants mutated state");
    const std::array<uint32_t, 8> scissors{1, 2, 3, 4, 5, 6, 7, 8};
    image.memory->Write(ctx, 0x20400, scissors.data(), sizeof(scissors));
    typed(35, {2, 1, 0x20400});
    check(refresh().scissor_count == 2 && refresh().scissors[7] == 8, "scissor state mismatch");
    const std::array<uint32_t, 6> viewport{10, 20, 320, 240, 0x3e800000, 0x3f400000};
    image.memory->Write(ctx, 0x20400, viewport.data(), sizeof(viewport));
    typed(34, {0x20400});
    typed(36, {0x3f000000, 0xbf000000});
    const auto& viewportState = refresh();
    check(viewportState.scissor_count == 0 && viewportState.vsc[58] == std::array<float, 4>{160, -120, 8388607.5f, 0} &&
      viewportState.vsc[59] == std::array<float, 4>{170.5f, 139.5f, 4194303.75f, 0}, "viewport constants mismatch");
    image.memory->Write(ctx, 0x20400, constants.data(), sizeof(constants));
    typed(37, {6, 0x20400});
    check(refresh().transforms[256][15] == 16, "transform type mapping mismatch");
    typed(37, {10, 0xffffffff});
    std::array<float, 17> material{};
    material[16] = 24;
    image.memory->Write(ctx, 0x20400, material.data(), sizeof(material));
    typed(38, {0x20400});
    check(refresh().material == material, "material mismatch");
    typed(39, {0x144});
    check(refresh().vs_handle == 0x144 && !refresh().vs, "FVF selection mismatch");
    typed(56, {7, gpu::kTssBumpEnvMat00, 0x3f800000});
    typed(57, {7, 0x11223344});
    typed(58, {7, 0x55667788});
    typed(59, {7, 0x1234});
    typed(60, {7, gpu::kTssMinFilter, 3});
    const auto textureState = refresh().tss[3];
    check(textureState[gpu::kTssBumpEnvMat00] == 0x3f800000 && textureState[gpu::kTssBorderColor] == 0x11223344 &&
      textureState[gpu::kTssColorKeyColor] == 0x55667788 && textureState[gpu::kTssTexCoordIndex] == 0x1234 &&
      textureState[gpu::kTssMinFilter] == 3, "texture stage masking or typed state mismatch");
    typed(60, {3, 32, 99});
    const uint32_t initialDeviceFlags = 0xa5000041;
    image.memory->Write(ctx, 0x4ab670 + 8, &initialDeviceFlags, 4);
    typed(63, {0x12});
    uint32_t constantMode;
    uint32_t deviceFlags;
    image.memory->Read(ctx, 0x4ab670 + 0x1928, &constantMode, 4);
    image.memory->Read(ctx, 0x4ab670 + 8, &deviceFlags, 4);
    check(constantMode == 2 && deviceFlags == (initialDeviceFlags | 0x200), "shader constant mode metadata mismatch");
    typed(63, {1});
    image.memory->Read(ctx, 0x4ab670 + 8, &deviceFlags, 4);
    check(deviceFlags == initialDeviceFlags, "shader constant mode flag clearing damaged device flags");
    ctx.esp = 0x22000;
    const std::array<uint32_t, 2> fixedFunctionMode{0xabcdef, 0x10};
    image.memory->Write(ctx, ctx.esp, fixedFunctionMode.data(), sizeof(fixedFunctionMode));
    rejects([&] { graphics->Dispatch(ctx, base, 63); });
    image.memory->Read(ctx, 0x4ab670 + 0x1928, &constantMode, 4);
    check(ctx.esp == 0x22000 && constantMode == 1, "unsupported fixed-function mode partially changed device metadata");
    const auto readWord = [&](uint32_t address) {
      uint32_t value;
      image.memory->Read(ctx, address, &value, 4);
      return value;
    };
    const std::array<uint32_t, 6> textureHeader{0x40001, 0x102000, 0, 0x11221, 0x00001001, 0};
    image.memory->Write(ctx, 0x20600, textureHeader.data(), sizeof(textureHeader));
    typed(46, {5, 0x20600});
    check(readWord(0x20600) == 0xc0001 && readWord(0x4ab670 + 0xf8c) == 0x20600 &&
      (readWord(0x4ab1c8) & 0x4800) == 0x4800 && refresh().textures[1] == 0x20600, "texture binding omitted native references or dirty flags");
    typed(46, {1, 0x20600});
    check(readWord(0x20600) == 0xc0001, "redundant texture binding changed references");
    typed(46, {1, 0});
    check(readWord(0x20600) == 0x40001, "texture unbind leaked internal reference");
    const std::array<uint32_t, 3> bufferHeader{1, 0x102000, 0};
    image.memory->Write(ctx, 0x20600, bufferHeader.data(), sizeof(bufferHeader));
    typed(47, {17, 0x20600, 24});
    check(readWord(0x20600) == 0x80001 && readWord(0x4adb10 + 12) == 24 &&
      readWord(0x4adb10 + 20) == 0x20600 && refresh().streams[1].vb == 0x20600, "stream binding omitted native metadata");
    typed(47, {1, 0x20600, 32});
    check(readWord(0x20600) == 0x80001 && refresh().streams[1].stride == 32, "redundant stream binding lost stride update");
    typed(48, {0x20600, 7});
    check(readWord(0x20600) == 0x100001 && readWord(0x4ab1cc) == 0x102000 &&
      readWord(0x4ab670 + 0x7a0) == 0x20600 && readWord(0x4ab670 + 0x1c) == 7, "index binding omitted native metadata");
    typed(48, {0, 9});
    typed(47, {1, 0, 0});
    check(readWord(0x20600) == 1 && refresh().index_data == 0 && refresh().base_vertex == 9, "buffer unbind leaked references");
    typed(45, {0, 0});
    check(refresh().rt.header == state.rt.header && !refresh().zs.header && refresh().viewport.w == 640 &&
      refresh().viewport.h == 480, "render target reuse or full viewport reset mismatch");
    typed(45, {state.rt.header, state.zs.header});
    const auto bindings = refresh().textures;
    ctx.esp = 0x22000;
    const std::array<uint32_t, 3> invalidTexture{0xabcdef, 0, 0x24000};
    image.memory->Write(ctx, ctx.esp, invalidTexture.data(), sizeof(invalidTexture));
    rejects([&] { graphics->Dispatch(ctx, base, 46); });
    check(ctx.esp == 0x22000 && refresh().textures == bindings, "invalid resource binding mutated state");
    const std::array<uint32_t, 8> declaration{0x20000000, 0x40320000, 0x82000011, 0x3f800000, 0x40000000, 0x40400000, 0x40800000, 0xffffffff};
    const std::array<uint32_t, 5> program{0x00012078, 0, 0x00200000, 0, 1};
    image.memory->Write(ctx, 0x20800, declaration.data(), sizeof(declaration));
    image.memory->Write(ctx, 0x20900, program.data(), sizeof(program));
    typed(49, {0x20800, 0x20900, 0x20700, 0});
    const uint32_t vertexShader = readWord(0x20700);
    typed(39, {vertexShader});
    check(refresh().vs && refresh().vs->handle == vertexShader && refresh().vs->program_type == 0x2078 &&
      refresh().vs->inputs[0].type == 0x32 && refresh().vsc[17] == constants[0], "created vertex shader selection omitted declaration constants or program");
    const auto ownedVertex = graphics->SnapshotState().vs;
    typed(50, {vertexShader});
    check(refresh().vs && ownedVertex->program.size() == 4, "deleting a bound shader invalidated its retained state");
    typed(39, {0x144});
    std::array<uint32_t, 60> definition{};
    for (uint32_t n = 0; n < definition.size(); ++n) { definition[n] = n + 100; }
    definition[57] = 0xfffffff0;
    definition[58] = 0xffffff1f;
    definition[59] = 0x10;
    image.memory->Write(ctx, 0x20800, definition.data(), sizeof(definition));
    typed(51, {0x20800, 0x20704});
    const uint32_t pixelShader = readWord(0x20704);
    typed(32, {pixelShader});
    check(refresh().ps && refresh().ps->handle == pixelShader && refresh().simple[gpu::kMPsFinalAbcd / 4] == definition[8] &&
      refresh().simple[gpu::kMPsCombinerCount / 4] == definition[53] && refresh().rs[gpu::kRsPsTextureModes] == definition[54],
      "created pixel shader did not load native combiner state");
    const std::array<float, 8> pixelConstants{1, 0.5f, 0, 0.25f, 0, 1, 0.25f, 1};
    image.memory->Write(ctx, 0x20900, pixelConstants.data(), sizeof(pixelConstants));
    typed(33, {0, 0x20900, 2});
    check(refresh().simple[gpu::kMPsConstant0 / 4] == 0x40ff8000 &&
      refresh().simple[gpu::kMPsConstant1 / 4 + 1] == 0xff00ff40 && refresh().simple[gpu::kMPsFinalConstant1 / 4] == 0xff00ff40,
      "pixel shader constant mappings did not update native combiner words");
    typed(33, {16, 0xffffffff, UINT32_MAX});
    typed(52, {pixelShader});
    check(refresh().ps && refresh().ps->def[59] == 0x10, "deleting bound pixel shader lost owned definition");
    typed(32, {0});
    ctx.esp = 0x22000;
    const std::array<uint32_t, 2> unknownShader{0xabcdef, pixelShader};
    image.memory->Write(ctx, ctx.esp, unknownShader.data(), sizeof(unknownShader));
    rejects([&] { graphics->Dispatch(ctx, base, 32); });
    check(ctx.esp == 0x22000 && !refresh().ps, "deleted shader handle was accepted");
    const std::array<uint32_t, 2> block{0xabcdef, state.rt.header};
    ctx.esp = 0x22000;
    image.memory->Write(ctx, ctx.esp, block.data(), sizeof(block));
    check(!graphics->Dispatch(ctx, base, 8) && ctx.esp == 0x22008, "fresh CPU resource wait did not complete");
    const std::array<uint32_t, 7> lock{0xabcdef, state.rt.header, 0, 0, 0x20300, 0, 0};
    ctx.esp = 0x22000;
    image.memory->Write(ctx, ctx.esp, lock.data(), sizeof(lock));
    check(graphics->Dispatch(ctx, base, 9) && ctx.esp == 0x22000, "fresh CPU surface lock was not delegated unchanged");
    const uint32_t fvf = 0x144;
    const uint32_t texture = 0x20400;
    const uint32_t filter = 2;
    const std::array<uint32_t, 3> stream{24, 0, 0x20500};
    image.memory->Write(ctx, 0x4ab670 + 0x798, &fvf, 4);
    image.memory->Write(ctx, 0x4ab670 + 0xf88, &texture, 4);
    image.memory->Write(ctx, 0x4adb10, stream.data(), sizeof(stream));
    image.memory->Write(ctx, 0x4ab1d0 + gpu::kTssMinFilter * 4, &filter, 4);
    ctx.esp = 0x22000;
    const std::array<uint32_t, 7> clear{0xabcdef, 0, 0, gpu::kClearTarget | gpu::kClearZ, 0x12345678, 0x3f800000, 0};
    image.memory->Write(ctx, ctx.esp, clear.data(), sizeof(clear));
    if (rendering) {
      sub_0049CE00(ctx, base);
      check(probe->clears == 1 && probe->flags == (gpu::kClearTarget | gpu::kClearZ) && probe->state.zs.header == state.zs.header,
        "rendering clear was not delegated with depth state");
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, block.data(), sizeof(block));
      check(!graphics->Dispatch(ctx, base, 8) && probe->syncs == 1 && ctx.esp == 0x22008,
        "GPU-written block did not synchronize through renderer");
      auto lateLock = lock;
      lateLock[6] = 0x20;
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, lateLock.data(), sizeof(lateLock));
      check(graphics->Dispatch(ctx, base, 9) && probe->lateSyncs == 1 && ctx.esp == 0x22000,
        "GPU-written lock bypassed real backend synchronization");
      const std::array<uint32_t, 4> draw{0xabcdef, 5, 0, 3};
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, draw.data(), sizeof(draw));
      sub_004A61A0(ctx, base);
      check(ctx.esp == 0x22010 && probe->draws == 1, "exported nonindexed draw was not delegated");
      const std::array<uint16_t, 3> indices{2, 0, 1};
      image.memory->Write(ctx, 0x20300, indices.data(), sizeof(indices));
      const std::array<uint32_t, 4> indexed{0xabcdef, 5, 3, 0x20300};
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, indexed.data(), sizeof(indexed));
      sub_004A6240(ctx, base);
      check(ctx.esp == 0x22010 && probe->draws == 2 &&
        probe->indices == std::vector<uint16_t>({2, 0, 1}), "indexed draw bytes were not delegated");
      const auto draws = probe->draws;
      auto invalid = indexed;
      invalid[3] = 0x23ffe;
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, invalid.data(), sizeof(invalid));
      rejects([&] { sub_004A6240(ctx, base); });
      check(probe->draws == draws && ctx.esp == 0x22000, "invalid indices reached rendering backend");
      const std::array<uint32_t, 2> swap{0xabcdef, 0};
      ctx.esp = 0x22000;
      image.memory->Write(ctx, ctx.esp, swap.data(), sizeof(swap));
      probe->rejectPresent = true;
      rejects([&] { sub_0049CC50(ctx, base); });
      check(ctx.esp == 0x22000 && graphics->Snapshot().resources[1].data == snapshot.resources[1].data, "failed Present mutated guest swap metadata");
      probe->rejectPresent = false;
      sub_0049CC50(ctx, base);
      check(ctx.esp == 0x22008 && ctx.eax == 1 && probe->presents == 1 && probe->front.header == state.rt.header,
        "genuine renderer Present or Swap ABI failed");
      const auto rotated = graphics->Snapshot();
      check(rotated.resources[0].data == snapshot.resources[1].data && rotated.resources[1].data == snapshot.resources[0].data &&
        rotated.resources[2].data == snapshot.resources[2].data && graphics->State().rt.data == (rotated.resources[1].data & 0xfffffff),
        "native color-only payload rotation failed");
      ctx.esp = 0x22000;
      sub_0049CC50(ctx, base);
      check(ctx.eax == 2 && graphics->Snapshot().resources[1].data == snapshot.resources[1].data,
        "second native Swap did not restore payload ring");
      const std::array<uint32_t, 1> persistFrame{0xabcdef};
      image.memory->Write(ctx, 0x22000, persistFrame.data(), sizeof(persistFrame));
      ctx.esp = 0x22000;
      probe->rejectSync = true;
      rejects([&] { graphics->Dispatch(ctx, base, 15); });
      check(ctx.esp == 0x22000 && !graphics->PersistedDisplaySnapshot(), "failed persistence published success or pixels");
      probe->rejectSync = false;
      std::vector<uint8_t> pixels(640 * 480 * 4, 0x3a);
      probe->onSync = [&](uint32_t address, uint32_t bytes) {
        check(bytes == pixels.size(), "persist readback size differs from owned front surface");
        image.memory->Write(ctx, address, pixels.data(), pixels.size());
      };
      check(!graphics->Dispatch(ctx, base, 15) && ctx.esp == 0x22004 && ctx.eax == 0,
        "PersistDisplay did not complete native stdcall ABI");
      const auto persisted = graphics->PersistedDisplaySnapshot();
      check(persisted && persisted->pixels == pixels && persisted->format == snapshot.resources[0].format &&
        persisted->size == snapshot.resources[0].size, "persisted display did not own real synchronized front pixels");
      const uint8_t changed = 0xee;
      image.memory->Write(ctx, snapshot.resources[0].data, &changed, 1);
      check(graphics->PersistedDisplaySnapshot()->pixels.front() == 0x3a, "persisted frame borrowed guest storage");
      pixels.front() = 0x6b;
      ctx.esp = 0x22000;
      check(!graphics->Dispatch(ctx, base, 15) && graphics->PersistedDisplaySnapshot()->pixels == pixels &&
        persisted->pixels.front() == 0x3a, "repeated persistence retained stale pixels or changed a previous snapshot");
      const uint32_t invalidSize = UINT32_MAX;
      image.memory->Write(ctx, snapshot.resources[0].header + 16, &invalidSize, 4);
      ctx.esp = 0x22000;
      const auto syncs = probe->syncs;
      rejects([&] { graphics->Dispatch(ctx, base, 15); });
      check(probe->syncs == syncs && ctx.esp == 0x22000 && graphics->PersistedDisplaySnapshot()->pixels == pixels,
        "invalid persistence metadata reached GPU or replaced the valid frame");
      image.memory->Write(ctx, snapshot.resources[0].header + 16, &snapshot.resources[0].size, 4);
      probe->onSync = {};

    } else {
      rejects([&] { sub_0049CE00(ctx, base); });
      const auto stopped = graphics->Snapshot();
      check(stopped.events[stopped.count - 1].arguments == std::array<uint32_t, 6>{17, 0, 0, gpu::kClearTarget | gpu::kClearZ, 0x12345678, 0x3f800000},
        "real SDK stop discarded scalar boundary evidence");
      rejects([&] { graphics->Backend().Clear(state, 0, nullptr, gpu::kClearTarget, 0xabcdef, 1, 0); });
      check(ctx.esp == 0x22000, "state capture completed Clear");
    }
    const auto captured = graphics->SnapshotState();
    check(captured.vs_handle == fvf && captured.textures[0] == texture && captured.streams[0].vb == stream[2] &&
      captured.streams[0].stride == stream[0] && captured.tss[0][gpu::kTssMinFilter] == filter,
      "live guest frontend metadata did not reach the portable snapshot");
    uint32_t untouched;
    image.memory->Read(ctx, snapshot.resources[1].data, &untouched, 4);
    check(untouched == 0, "frontend performed an unrendered CPU write");
    uint32_t push;
    image.memory->Read(ctx, 0x4ab670, &push, 4);
    const std::array<uint32_t, 7> stateOnly{0x00041ea4, 17, 0x00100b80, 0x3f800000, 0x40000000, 0x40400000, 0x40800000};
    image.memory->Write(ctx, push, stateOnly.data(), sizeof(stateOnly));
    push += sizeof(stateOnly);
    image.memory->Write(ctx, 0x4ab670, &push, 4);
    ctx.esp = 0x22000;
    image.memory->Write(ctx, ctx.esp, block.data(), sizeof(block));
    check(!graphics->Dispatch(ctx, base, 8) && graphics->State().vsc[17] == std::array<float, 4>{1, 2, 3, 4},
      "verified state-only pushes did not update portable constants before resource wait");
    if (rendering) {
      uint32_t threshold;
      image.memory->Read(ctx, 0x4ab674, &threshold, 4);
      const uint32_t scratch = threshold - 0xf000;
      for (uint32_t n = 0; n < 128; ++n) {
        ctx.esp = 0x22000;
        check(!graphics->Dispatch(ctx, base, 4) && ctx.esp == 0x22004 && ctx.eax == scratch,
          "MakeSpace did not return and rewind owned scratch storage");
        image.memory->Read(ctx, 0x4ab670, &push, 4);
        check(push == scratch && graphics->Snapshot().commands.empty() &&
          graphics->State().vsc[17] == std::array<float, 4>{1, 2, 3, 4},
          "scratch recycling lost decoded state or retained command history");
        image.memory->Write(ctx, push, stateOnly.data(), sizeof(stateOnly));
        push += sizeof(stateOnly);
        image.memory->Write(ctx, 0x4ab670, &push, 4);
      }
    }
    const std::array<uint32_t, 2> unsubmitted{0x000417fc, 99};
    image.memory->Write(ctx, push, unsubmitted.data(), sizeof(unsubmitted));
    push += sizeof(unsubmitted);
    image.memory->Write(ctx, 0x4ab670, &push, 4);
    ctx.esp = 0x22000;
    image.memory->Write(ctx, ctx.esp, lock.data(), sizeof(lock));
    rejects([&] { graphics->Dispatch(ctx, base, 9); });
    rejects([&] { graphics->Dispatch(ctx, base, 9); });
    if (rendering) {
      const auto flushes = probe->flushes;
      rejects([&] { graphics->Dispatch(ctx, base, 7); });
      check(probe->flushes == flushes, "unverified guest draws reached idle drain");
      rejects([&] { graphics->Dispatch(ctx, base, 4); });
      uint32_t cursor;
      image.memory->Read(ctx, 0x4ab670, &cursor, 4);
      check(cursor == push, "MakeSpace discarded unverified guest commands");
    }
    check(ctx.esp == 0x22000, "unsubmitted guest pushes were acknowledged as available CPU data");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  runtime.Stop();
  graphics->Shutdown();
  check(backendLifetime.expired() && !graphics->PersistedDisplaySnapshot(), "GPU backend or persisted pixels outlived joined cleanup");
  graphics->Shutdown();
  image.memory.reset();
  defaults = 0;
  return 0;
}

int contracts() {
  browser::PreparedImage image;
  browser::Runtime runtime(std::array<X86FuncMapping, 4>{{{0xf11000, sub_00F11000}, {0xf12000, sub_00F12000}, {0xf13000, sub_00F13000}, {0x4a1a80, sub_004A1A80}}}, {});
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 0x1000);
  image.memory->Ensure(0x20000, 0x4000);
  image.memory->Ensure(0x4a8000, 0x1000);
  image.memory->Ensure(0x4ab000, 0x4000);
  const uint8_t formatBits = 32;
  image.memory->Write({}, 0x4a8df8 + 18, &formatBits, 1);
  image.image.kernel_thunk = 0x10000;
  const std::array<uint32_t, 1> terminator{};
  image.memory->Write({}, 0x10000, terminator.data(), sizeof(terminator));
  browser::ContiguousMemoryAccess allocations;
  browser::QueryContiguousMemoryUsage usage;
  browser::InstallContiguousMemory(runtime, *image.memory, &usage, &allocations);
  uint32_t allocationCalls = 0;
  uint32_t failAllocation = 0;
  const auto allocate = allocations.allocate;
  allocations.allocate = [&](const X86Context& ctx, uint32_t bytes, uint32_t alignment) {
    ++allocationCalls;
    if (allocationCalls == failAllocation) { return uint32_t(0); }
    return allocate(ctx, bytes, alignment);
  };
  auto graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, image, allocations);
  auto trace = browser::InstallSdkProvider(runtime, image, browser::SdkPolicy::UnavailableXNet, graphics);
  browser::BindImageImports(image, runtime, {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x22000;
  context.ebx = 0x12345678;
  const std::array<uint32_t, 7> frame = {0xabcdef, 0, 1, 0, 0x40, 0x20000, 0x20100};
  const std::array<uint32_t, 13> presentation = {640, 480, 18, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  image.memory->Write(context, context.esp, frame.data(), sizeof(frame));
  image.memory->Write(context, frame[5], presentation.data(), sizeof(presentation));
  rejects([&] { graphics->CreateDevice(context, image.memory->Base()); });
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto before = ctx;
    {
      auto lease = image.memory->Borrow();
      rejects([&] { sub_00F13000(ctx, base); });
    }
    rejects([&] { sub_00F13000(ctx, nullptr); });
    auto invalid = frame;
    invalid[6] = 0x4ab670;
    image.memory->Write(ctx, ctx.esp, invalid.data(), sizeof(invalid));
    rejects([&] { sub_00F13000(ctx, base); });
    invalid[6] = 0x80100000;
    image.memory->Write(ctx, ctx.esp, invalid.data(), sizeof(invalid));
    rejects([&] { sub_00F13000(ctx, base); });
    check(usage(ctx, *image.memory) == 0, "unsafe physical output triggered allocation");
    image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    failAllocation = allocationCalls + 3;
    rejects([&] { sub_00F13000(ctx, base); });
    check(usage(ctx, *image.memory) == 0 && !graphics->Snapshot().initialized, "allocation failure leaked published resources");
    failAllocation = 0;
    defaultMode = 1;
    bool originalFailure = false;
    try { sub_00F13000(ctx, base); } catch (const browser::Fault& error) { originalFailure = error.address == 0x777; }
    check(originalFailure && ctx.esp == before.esp && usage(ctx, *image.memory) == 0, "retained default failure was not rolled back");
    uint32_t rolledBack;
    image.memory->Read(ctx, 0x4ab670 + 0xc60, &rolledBack, 4);
    check(rolledBack == 0 && !graphics->Snapshot().initialized, "failed default CPU metadata escaped");
    defaultMode = 0;
    sub_00F13000(ctx, base);
    check(ctx.eax == 0 && ctx.esp == before.esp + 28 && ctx.ebx == before.ebx, "CreateDevice ABI incorrect");
    check(defaults == 2 && originals == 0, "defaults or original identity incorrect");
    uint32_t output;
    image.memory->Read(ctx, frame[6], &output, 4);
    check(output == 0x4ab670, "device output missing");
    const auto snapshot = graphics->Snapshot();
    check(snapshot.initialized && snapshot.resources.size() == 2 && snapshot.commandWords == 2, "backed device resources or command capture missing");
    check(snapshot.commands == std::vector<uint32_t>({0x00040300, 1}), "retained default command bytes were discarded or synthesized");
    check(usage(ctx, *image.memory) > 0, "graphics bypassed contiguous ledger");
    bool contextualFault = false;
    try { graphics->Dispatch(ctx, base, 7); } catch (const browser::Fault& error) {
      contextualFault = error.context->host == &runtime && error.context->esp == ctx.esp && error.context->ebx == ctx.ebx;
    }
    check(contextualFault, "unavailable SDK result lost genuine CPU context");
    uint32_t validCursor;
    image.memory->Read(ctx, 0x4ab670, &validCursor, 4);
    const uint32_t escapedCursor = validCursor - 8 + 0x10004;
    image.memory->Write(ctx, 0x4ab670, &escapedCursor, 4);
    rejects([&] { graphics->Dispatch(ctx, base, 7); });
    uint32_t unchangedCursor;
    image.memory->Read(ctx, 0x4ab670, &unchangedCursor, 4);
    check(unchangedCursor == escapedCursor && graphics->Snapshot().commands == snapshot.commands, "capture overflow rewound or discarded retained commands");
    image.memory->Write(ctx, 0x4ab670, &validCursor, 4);
    const auto& surface = snapshot.resources[1];
    std::array<uint32_t, 6> header;
    image.memory->Read(ctx, surface.header, header.data(), sizeof(header));
    check(header[0] == 0xd0001 && header[1] == (surface.data & 0xfffffff) && header[3] == 0x11221 && header[4] == (639u | (479u << 12) | (39u << 24)), "surface metadata incorrect");
    uint32_t alias = 0xf0000000 | header[1];
    const uint32_t upload = 0xdeadbeef;
    image.memory->Write(ctx, alias, &upload, 4);
    uint32_t real;
    image.memory->Read(ctx, surface.data, &real, 4);
    check(real == upload, "surface physical alias is not backed");
    auto& backend = graphics->Backend();
    backend.Sync(alias, 4);
    rejects([&] { backend.Sync(0xa0000000 | header[1], 4); });
    ctx.esp = 0x22000;
    const std::array<uint32_t, 7> clearFrame{0xabcdef, 0, 0, gpu::kClearTarget, 0, 0, 0};
    image.memory->Write(ctx, ctx.esp, clearFrame.data(), sizeof(clearFrame));
    sub_0049CE00(ctx, base);
    check(ctx.esp == 0x2201c && ctx.eax == 0, "Clear ABI incorrect");
    image.memory->Read(ctx, alias, &real, 4);
    check(real == 0, "CPU clear did not change actual backing bytes");
    backend.Sync(alias, 4);
    ctx.esp = 0x22000;
    const std::array<uint32_t, 7> lockFrame{0xabcdef, surface.header, 0, 0, 0x20200, 0, 0x40};
    image.memory->Write(ctx, ctx.esp, lockFrame.data(), sizeof(lockFrame));
    sub_0049DE60(ctx, base);
    check(ctx.esp == 0x2201c, "CPU clean lock did not retain original");
    std::array<uint32_t, 2> locked;
    image.memory->Read(ctx, 0x20200, locked.data(), sizeof(locked));
    check(locked[0] == 2560 && locked[1] == alias, "genuine retained Lock2D did not publish real pitch and CPU alias");
    backend.Draw(graphics->State(), 4, 0, 3, nullptr);
    ctx.esp = 0x22000;
    rejects([&] { sub_0049DE60(ctx, base); });
    rejects([&] { backend.Sync(alias, 4); });
    backend.Sync(snapshot.resources[0].data, 4);
    const uint32_t fence = backend.Fence();
    check(backend.FencePending(fence), "diagnostic commands completed fake fence");
    rejects([&] { backend.WaitFence(fence); });
    rejects([&] { backend.WaitResource(surface.header); });
    rejects([&] { backend.WaitGpuFrame(0); });
    const std::array<uint32_t, 2> swapFrame{0xabcdef, 0};
    ctx.esp = 0x22000;
    image.memory->Write(ctx, ctx.esp, swapFrame.data(), sizeof(swapFrame));
    sub_0049CC50(ctx, base);
    check(ctx.eax == 1 && ctx.esp == 0x22008, "Present request ABI or real swap counter incorrect");
    const auto rotated = graphics->Snapshot();
    check(rotated.resources[0].data == snapshot.resources[1].data && rotated.resources[1].data == snapshot.resources[0].data, "surface storage did not genuinely rotate");
    check(rotated.resources[0].unavailable && !rotated.resources[1].unavailable, "GPU result availability did not follow storage rotation");
    backend.Sync(rotated.resources[1].data, 4);
    rejects([&] { backend.Sync(rotated.resources[0].data, 4); });
    const auto state = graphics->State();
    for (uint32_t n = 0; n < 80; ++n) { backend.Present(state.rt, false); }
    const auto bounded = graphics->Snapshot();
    check(bounded.count == 64 && bounded.total > bounded.count, "recorder is not bounded");
    rejects([&] { graphics->CreateDevice(ctx, base); });
    const auto calls = originals.load();
    rejects([&] { __imp__sub_00F13000(ctx, base); });
    check(originals == calls + 1, "retained original disappeared");
    auto sharedState = std::make_shared<gpu::State>(graphics->State());
    runtime.Launch(ctx, base, [graphics, sharedState](X86Context&, uint8_t*) {
      for (uint32_t n = 0; n < 80; ++n) { graphics->Backend().Present(sharedState->rt, false); }
    });
    for (uint32_t n = 0; n < 80; ++n) { graphics->Backend().Present(sharedState->rt, false); }
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(trace->Snapshot().events[0].behavior == browser::SdkBehavior::Diagnostic, "SDK ledger not integrated");
  runtime.Stop();
  rejects([&] { graphics->Backend().Sync(0x80100000, 4); });
  const auto final = graphics->Snapshot();
  check(final.count == 64 && final.total > 240, "concurrent recorder lost requests");
  for (size_t n = 1; n < final.count; ++n) { check(final.events[n].sequence == final.events[n - 1].sequence + 1, "concurrent recorder sequence incoherent"); }
  {
    browser::PreparedImage cancelledImage;
    browser::Runtime cancelledRuntime(std::array<X86FuncMapping, 4>{{{0xf11000, sub_00F11000}, {0xf12000, sub_00F12000}, {0xf13000, sub_00F13000}, {0x4a1a80, sub_004A1A80}}}, {});
    cancelledImage.memory = std::make_unique<browser::Memory>();
    cancelledImage.memory->Ensure(0x10000, 0x1000);
    cancelledImage.memory->Ensure(0x20000, 0x4000);
    cancelledImage.memory->Ensure(0x4ab000, 0x4000);
    cancelledImage.image.kernel_thunk = 0x10000;
    cancelledImage.memory->Write({}, 0x10000, terminator.data(), sizeof(terminator));
    browser::ContiguousMemoryAccess cancelledAccess;
    browser::InstallContiguousMemory(cancelledRuntime, *cancelledImage.memory, nullptr, &cancelledAccess);
    auto cancelledGraphics = std::make_shared<browser::DiagnosticGraphics>(cancelledRuntime, cancelledImage, cancelledAccess);
    browser::InstallSdkProvider(cancelledRuntime, cancelledImage, browser::SdkPolicy::Observer, cancelledGraphics);
    browser::BindImageImports(cancelledImage, cancelledRuntime, {});
    X86Context cancelledContext{};
    cancelledContext.host = &cancelledRuntime;
    cancelledContext.esp = 0x22000;
    cancelledImage.memory->Write(cancelledContext, 0x22000, frame.data(), sizeof(frame));
    cancelledImage.memory->Write(cancelledContext, 0x20000, presentation.data(), sizeof(presentation));
    defaultMode = 2;
    std::atomic<bool> cancelled = false;
    cancelledRuntime.Launch(cancelledContext, cancelledImage.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      try { sub_00F13000(ctx, base); } catch (const browser::Cancelled&) { cancelled = true; throw; }
    });
    if (const auto failure = cancelledRuntime.Join()) { std::rethrow_exception(failure); }
    check(cancelled && !cancelledGraphics->Snapshot().initialized && cancelledGraphics->Snapshot().resources.empty(), "cancelled defaults published a device or replaced cancellation");
    uint32_t cancelledOutput;
    cancelledImage.memory->Read({}, 0x20100, &cancelledOutput, 4);
    check(cancelledOutput == 0, "cancelled device wrote output");
    cancelledRuntime.Stop();
    defaultMode = 0;
  }
  std::puts("diagnostic-graphics-contracts-ok");
  return 0;
}
int releaseContracts() {
  browser::PreparedImage image;
  browser::Runtime runtime(std::array<X86FuncMapping, 4>{{{0xf11000, sub_00F11000}, {0xf12000, sub_00F12000}, {0xf13000, sub_00F13000}, {0x49bd80, sub_0049BD80}}}, {});
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 0x1000);
  image.memory->Ensure(0x20000, 0x4000);
  image.image.kernel_thunk = 0x10000;
  browser::ContiguousMemoryAccess access;
  browser::InstallContiguousMemory(runtime, *image.memory, nullptr, &access);
  auto backend = std::make_unique<ProbeBackend>();
  auto* probe = backend.get();
  auto graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, image, access, browser::GraphicsMode::Rendering, std::move(backend));
  browser::InstallSdkProvider(runtime, image, browser::SdkPolicy::Observer, graphics);
  browser::BindImageImports(image, runtime, {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x22000;
  destroyedResources.clear();
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto create = [&](uint32_t header, uint32_t common, uint32_t data, uint32_t parent = 0) {
      image.memory->Ensure(header, 0x1000);
      const std::array<uint32_t, 6> words{common, data, 0, 0, 0, parent};
      image.memory->Write(ctx, header, words.data(), sizeof(words));
    };
    const auto frame = [&](uint32_t header) {
      ctx.esp = 0x22000;
      const std::array<uint32_t, 2> words{0xabcdef, header};
      image.memory->Write(ctx, ctx.esp, words.data(), sizeof(words));
    };
    create(0x30000, 0x40002, 0x80123000);
    frame(0x30000);
    sub_0049BD80(ctx, base);
    check(ctx.eax == 1 && ctx.esp == 0x22008 && probe->released.empty(), "nonfinal external release retired storage or changed ABI");
    frame(0x30000);
    sub_0049BD80(ctx, base);
    check(ctx.eax == 0 && ctx.esp == 0x22008 && probe->released == std::vector<std::array<uint32_t, 2>>{{0x30000, 0x80123000}}, "genuine final release did not notify after header decommit");
    create(0x31000, 0xc0001, 0x123000);
    frame(0x31000);
    sub_0049BD80(ctx, base);
    check(ctx.eax == 0 && probe->released.size() == 1 && image.memory->IsCommitted(0x31000), "external zero retired internally retained alias");
    frame(0x31000);
    sub_0049BF30(ctx, base);
    check(probe->released.size() == 2 && probe->released.back()[0] == 0x31000, "internal final release failed retirement");
    create(0x32000, 0x40001, 0x123000);
    create(0x33000, 0x50001, 0x123000, 0x32000);
    frame(0x33000);
    sub_0049BD80(ctx, base);
    check(ctx.eax == 0 && ctx.esp == 0x22008 && probe->released.size() == 4 && probe->released[2][0] == 0x32000 && probe->released[3][0] == 0x33000, "nested parent final release lost owned snapshot");
    create(0x34000, 0x40001, 0x123000);
    frame(0x34000);
    destroyFails = true;
    rejects([&] { sub_0049BD80(ctx, base); });
    destroyFails = false;
    check(probe->released.size() == 4, "failed original release retired storage");
    frame(0x34000);
    sub_0049BD80(ctx, base);
    check(probe->released.size() == 5, "exception leaked release marker");
    create(0x37000, 0xc0000, 0x123000);
    create(0x38000, 0xd0000, 0x123000, 0x37000);
    frame(0x38000);
    sub_0049BF30(ctx, base);
    check(probe->released.size() == 7 && probe->released[5][0] == 0x37000 && probe->released[6][0] == 0x38000, "internal final release lost direct parent destruction");
    create(0x39000, 0x40001, 0x123000);
    frame(0x39000);
    const auto finalBefore = ctx;
    __imp__sub_0049BD80(ctx, base);
    const auto finalExpected = ctx;
    create(0x39000, 0x40001, 0x123000);
    ctx = finalBefore;
    sub_0049BD80(ctx, base);
    check(!std::memcmp(&ctx, &finalExpected, sizeof(ctx)), "final release observer altered genuine destructor effects or ABI");
    const auto releasesBeforeOverflow = probe->released.size();
    for (uint32_t n = 0; n < 34; ++n) {
      create(0x40000 + n * 0x1000, 0x50001, 0x123000, n == 33 ? 0 : 0x41000 + n * 0x1000);
    }
    frame(0x40000);
    rejects([&] { sub_0049BD80(ctx, base); });
    check(probe->released.size() == releasesBeforeOverflow, "release nesting overflow retired incomplete originals");
    create(0x39000, 0x40001, 0x123000);
    frame(0x39000);
    sub_0049BD80(ctx, base);
    check(probe->released.size() == releasesBeforeOverflow + 1, "release nesting overflow leaked worker markers");
    frame(0x34000);
    rejects([&] { browser::Runtime::ObserveSdkEntry(ctx, base, 66); });
    rejects([&] { browser::Runtime::ObserveSdkEntry(ctx, base, 67); });
    rejects([&] { browser::Runtime::ObserveSdkEntry(ctx, base, 68); });
    rejects([&] { browser::Runtime::ObserveSdkEntry(ctx, base, 69); });
    for (const uint32_t stack : {0x23ffcu, 0xfffffffcu}) {
      ctx.esp = stack;
      rejects([&] { sub_0049BD80(ctx, base); });
    }
    create(0x35000, 0x40002, 0x123000);
    create(0x36000, 0x40002, 0x123000);
    frame(0x35000);
    const auto before = ctx;
    __imp__sub_0049BD80(ctx, base);
    const auto expected = ctx;
    ctx = before;
    frame(0x36000);
    sub_0049BD80(ctx, base);
    check(!std::memcmp(&ctx, &expected, sizeof(ctx)), "release wrapper altered genuine ABI registers or flags");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  const auto releasedBeforeWorkers = probe->released.size();
  std::atomic<uint32_t> ready = 0;
  for (uint32_t n = 0; n < 2; ++n) {
    const uint32_t header = 0x80000 + n * 0x1000;
    image.memory->Ensure(header, 0x1000);
    const std::array<uint32_t, 6> words{0x40001, 0x123000};
    image.memory->Write({}, header, words.data(), sizeof(words));
    auto current = context;
    current.esp += n * 0x100;
    const std::array<uint32_t, 2> frame{0xabcdef, header};
    image.memory->Write({}, current.esp, frame.data(), sizeof(frame));
    runtime.Launch(current, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      ++ready;
      while (ready.load() < 2) { std::this_thread::yield(); }
      const auto stack = ctx.esp;
      sub_0049BD80(ctx, base);
      check(ctx.eax == 0 && ctx.esp == stack + 8, "concurrent release used another worker marker");
    });
  }
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(probe->released.size() == releasedBeforeWorkers + 2, "concurrent release lost a worker retirement");
  graphics->Shutdown();
  return 0;
}
int main() {
  try { presentCheckpointContracts(1); presentCheckpointContracts(3); presentCheckpointContracts(3, true); presentCheckpointContracts(0); releaseContracts(); checkpointContracts(); captureContracts(false); captureContracts(true); captureContracts(true, true); return contracts(); } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
