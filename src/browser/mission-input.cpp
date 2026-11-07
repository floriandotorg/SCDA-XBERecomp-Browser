#include "browser/mission-input.h"
#include "browser/mission-clock.h"
#include "browser/hooks.h"
#include "cpu/x86_ops.h"
#include <bit>
#include <cmath>
#include <optional>
#include <chrono>

X86_FUNC(__imp__sub_001D2A4E);
X86_FUNC(__imp__sub_001CDA8A);
BROWSER_GAME_CHAIN(00039AD4)
X86_FUNC(sub_00039AD4) {
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 135)) { game_sub_00039AD4(ctx, base); }
}
X86_FUNC(sub_001D2A4E) {
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 133)) { __imp__sub_001D2A4E(ctx, base); }
}
X86_FUNC(sub_001CDA8A) {
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 134)) { __imp__sub_001CDA8A(ctx, base); }
}

namespace browser {
thread_local MissionInput* MissionInput::stepping_ = nullptr;
thread_local MissionInput* MissionInput::suppressed_ = nullptr;
thread_local uint32_t MissionInput::viewport_ = 0;

MissionInput::StepSnapshot MissionInput::SnapshotSteps() const {
  return {steps_.load(), skippedDraws_.load(), updateMicroseconds_.load(), renderMicroseconds_.load(), backlogTicks_.load()};
}

void MissionInput::ConfigureDevelopmentStart() {
  if (image_.importsBound || runtime_.HasPublishedImage() || runtime_.ActiveWorkers() || Memory::HasBorrow()) {
    throw Fault({}, 0, 0, "development start requires a cold unpublished mission runtime");
  }
  skipVideos_ = true;
}

MissionInput::Scene MissionInput::ReadScene(const X86Context& context) {
  auto lease = memory_->Borrow();
  const auto word = [&](uint32_t address, uint32_t offset = 0) {
    if (!address || uint64_t(address) + offset + 4 > uint64_t(UINT32_MAX) + 1) { throw Fault(context, address, 4, "mission scene pointer overflow"); }
    return memory_->Load<uint32_t>(context, address + offset);
  };
  Scene scene;
  if (!context.ecx) { return scene; }
  const uint32_t table = word(context.ecx);
  if (word(table, 0x5c) != 0x1d2a4e || word(table, 0x7c) != 0x1cda8a) { return scene; }
  const uint32_t client = word(context.ecx, 0x48);
  const uint32_t viewports = client ? word(client, 0x30) : 0;
  scene.viewport = viewports ? word(viewports) : 0;
  const uint32_t actor = scene.viewport ? word(scene.viewport, 0x34) : 0;
  scene.level = actor ? word(actor, 0xf4) : 0;
  if (!scene.level) { return scene; }
  scene.seconds = std::bit_cast<float>(word(scene.level, 0x3bc));
  if (!std::isfinite(scene.seconds) || scene.seconds < 0) { return scene; }
  const uint32_t interaction = word(scene.viewport, 0x70);
  const uint32_t manager = interaction ? word(interaction, 0x3c) : 0;
  const uint32_t id = manager ? word(manager, 0xc) : 0;
  const uint32_t magma = word(0x6a6638);
  if (!magma || !manager) { return scene; }
  const uint32_t contexts = word(magma, 0x3c), count = word(magma, 0x40);
  if (count > 16) { throw Fault(context, magma, count, "mission scene context budget exceeded"); }
  uint32_t first = 0, page = 0;
  for (uint32_t n = 0; contexts && n < count; ++n) {
    const uint32_t current = word(contexts, n * 4);
    const uint32_t stack = current ? word(current, 0x18) : 0;
    const uint32_t begin = stack ? word(stack, 4) : 0, end = stack ? word(stack, 8) : 0;
    if (!begin || end <= begin || end - begin > 512 || (end - begin) % 8) { continue; }
    if (word(current, 0x1c) == id) { page = word(end - 8); break; }
    if (!first) { first = word(end - 8); }
  }
  if (!page) { page = first; }
  scene.active = page && word(page, 0x1c) == 100 && !(*storage_)[2].load();
  return scene;
}

void MissionInput::Tick(X86Context& context, uint8_t* base) {
  if (stepping_ == this) { __imp__sub_001D2A4E(context, base); return; }
  const auto original = context;
  const auto scene = ReadScene(context);
  if (scene.level == previousLevel_ && scene.seconds < previousSeconds_) { stepper_.Reset(); }
  previousLevel_ = scene.level;
  previousSeconds_ = scene.seconds;
  const float originalDelta = memory_->Load<float>(context, context.esp + 4);
  const float maximumDelta = memory_->Load<float>(context, 0x4f64c8);
  MissionStepper::Plan plan;
  try { plan = stepper_.Advance(runtime_.TimerTime(context), scene.level, scene.active, originalDelta, maximumDelta); }
  catch (const std::runtime_error& error) { throw Fault(context, 0x1d2a4e, 0, error.what()); }
  struct Binding {
    MissionInput*& slot;
    MissionInput* previous;
    Binding(MissionInput*& slot, MissionInput* value) : slot(slot), previous(slot) { slot = value; }
    ~Binding() { slot = previous; }
  } binding(stepping_, this);
  struct Viewport {
    uint32_t& slot;
    uint32_t previous;
    Viewport(uint32_t& slot, uint32_t value) : slot(slot), previous(slot) { slot = value; }
    ~Viewport() { slot = previous; }
  } viewport(viewport_, scene.viewport);
  const auto start = std::chrono::steady_clock::now();
  const uint32_t inactive = (*storage_)[4].load();
  const uint64_t rendered = renderMicroseconds_.load();
  for (uint32_t n = 0; n < plan.steps; ++n) {
    runtime_.Checkpoint(context);
    context = original;
    memory_->Store(context, context.esp + 4, plan.seconds);
    {
      Binding rendering(suppressed_, n + 1 < plan.steps ? this : nullptr);
      __imp__sub_001D2A4E(context, base);
    }
    ++steps_;
    if (context.esp != original.esp + 8) { throw Fault(context, context.esp, 8, "mission tick stack cleanup changed"); }
    auto probe = context;
    probe.ecx = original.ecx;
    const auto after = ReadScene(probe);
    if (after.level != scene.level || after.seconds < scene.seconds || (scene.active && !after.active)) {
      stepper_.Reset();
      plan.backlogTicks = 0;
      break;
    }
  }
  backlogTicks_ = plan.backlogTicks;
  const uint64_t elapsed = MissionClock::ActiveMicroseconds(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), inactive, (*storage_)[4].load());
  const uint64_t drawing = renderMicroseconds_.load() - rendered;
  updateMicroseconds_.fetch_add(elapsed - std::min(elapsed, drawing));
}

void MissionInput::Draw(X86Context& context, uint8_t* base) {
  if (suppressed_ == this && memory_->Load<uint32_t>(context, context.esp + 4) == viewport_) {
    context.esp += 24;
    ++skippedDraws_;
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  const uint32_t inactive = (*storage_)[4].load();
  __imp__sub_001CDA8A(context, base);
  renderMicroseconds_.fetch_add(MissionClock::ActiveMicroseconds(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), inactive, (*storage_)[4].load()));
}

MissionInput::MissionInput(Runtime& runtime, PreparedImage& image, std::shared_ptr<SdkGraphics> controller, std::shared_ptr<SharedStorage> storage, SimulationObserver observer)
  : observer_(std::move(observer)), runtime_(runtime), image_(image), memory_(image.memory.get()), controller_(std::move(controller)), storage_(std::move(storage)) {
  if (!memory_ || !storage_ || image.importsBound || runtime.HasPublishedImage() || runtime.ActiveWorkers() || Memory::HasBorrow() ||
      (controller_ && !controller_->BelongsTo(runtime, image))) {
    throw Fault({}, 0, 0, "mission input requires its cold owned image and transport");
  }
}
bool MissionInput::BelongsTo(Runtime& runtime, PreparedImage& image) const {
  return &runtime == &runtime_ && &image == &image_ && memory_ == image.memory.get();
}
void MissionInput::ObserveSimulation(const X86Context& context) {
  if (!observer_ || (*storage_)[2].load() || !context.ecx) { return; }
  std::optional<SimulationSample> sample;
  {
    auto lease = memory_->Borrow();
    const auto read = [&](uint32_t address, uint32_t offset) {
      if (uint64_t(address) + offset + 4 > uint64_t(UINT32_MAX) + 1) { throw Fault(context, address, 4, "simulation sample pointer overflow"); }
      uint32_t value;
      memory_->Read(context, address + offset, &value, 4);
      return value;
    };
    const uint32_t actor = read(context.ecx, 0x34);
    if (!actor) { return; }
    const uint32_t level = read(actor, 0xf4);
    if (!level) { return; }
    const float seconds = std::bit_cast<float>(read(level, 0x3bc)), delta = std::bit_cast<float>(read(level, 0x3f4));
    if (!std::isfinite(seconds) || seconds < 0 || !std::isfinite(delta) || delta < 0 || delta > 10) { return; }
    sample = SimulationSample{level, seconds, delta};
  }
  observer_(*sample);
}
bool MissionInput::Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) {
  if (id < 132 || id > 135) { return controller_ ? controller_->Dispatch(ctx, base, id) : true; }
  if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
      base != memory_->Base() || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, ctx.esp, 0, "mission input requires its bound current worker without a lease");
  }
  runtime_.Checkpoint(ctx);
  if (id == 135) {
    if (!skipVideos_) { return true; }
    if (uint64_t(ctx.esp) + 24 > UINT32_MAX) { throw Fault(ctx, ctx.esp, 24, "development video frame overflow"); }
    memory_->CheckRange(ctx, ctx.esp, 24);
    ctx.eax = 0;
    ctx.esp += 24;
    return false;
  }
  if (id == 133) { Tick(ctx, base); return false; }
  if (id == 134) { Draw(ctx, base); return false; }
  // mouse look is mouse.cpp's (input-update.cpp): this entry only samples the simulation
  ObserveSimulation(ctx);
  return false;
}
}
