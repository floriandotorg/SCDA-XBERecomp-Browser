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
X86_FUNC(sub_0049CC50);
X86_FUNC(sub_0049A3D0);
X86_FUNC(sub_0049EFF0);
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
  uint32_t presents = 0;
  bool rejectPresent = false;
  gpu::Surface front;
  std::array<uint8_t, 768> gamma{};
  uint32_t gammaCalls = 0;
  std::vector<uint16_t> indices;
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
    check(address >= 0x80000000 && !bytes, "resource sync ABI mismatch");
    ++syncs;
  }
  void SyncLate(uint32_t address) override {
    check(address >= 0x80000000, "SyncLate did not receive checked virtual resource data");
    ++lateSyncs;
  }
  void Flush() override {}
  void SetGamma(const uint8_t* ramp) override {
    std::memcpy(gamma.data(), ramp, gamma.size());
    ++gammaCalls;
  }
};

int captureContracts(bool rendering, bool failInitialization = false) {
  browser::PreparedImage image;
  browser::Runtime runtime(std::array<X86FuncMapping, 4>{{{0xf11000, sub_00F11000}, {0xf12000, sub_00F12000}, {0xf13000, sub_00F13000}, {0x4a1a80, sub_004A1A80}}}, {});
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
    ctx.esp = 0x22000;
    ctx.ecx = 17;
    ctx.edx = 0x20300;
    rejects([&] { sub_0049E8B0(ctx, base); });
    check(ctx.esp == 0x22000 && graphics->State().vsc[17] == std::array<float, 4>{}, "unmirrored typed shader constant setter completed");
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
    const std::array<uint32_t, 2> unsubmitted{0x000417fc, 99};
    image.memory->Write(ctx, push, unsubmitted.data(), sizeof(unsubmitted));
    push += sizeof(unsubmitted);
    image.memory->Write(ctx, 0x4ab670, &push, 4);
    ctx.esp = 0x22000;
    image.memory->Write(ctx, ctx.esp, lock.data(), sizeof(lock));
    rejects([&] { graphics->Dispatch(ctx, base, 9); });
    rejects([&] { graphics->Dispatch(ctx, base, 9); });
    check(ctx.esp == 0x22000, "unsubmitted guest pushes were acknowledged as available CPU data");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  runtime.Stop();
  graphics->Shutdown();
  check(backendLifetime.expired(), "GPU backend outlived its joined image memory owner");
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
int main() {
  try { checkpointContracts(); captureContracts(false); captureContracts(true); captureContracts(true, true); return contracts(); } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
