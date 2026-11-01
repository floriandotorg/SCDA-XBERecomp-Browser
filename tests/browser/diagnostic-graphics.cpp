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
X86_FUNC(sub_004A1C00);
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
    sub_004A1C00(ctx, base);
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
  try { checkpointContracts(); return contracts(); } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
