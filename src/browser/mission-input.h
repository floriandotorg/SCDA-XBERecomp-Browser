#pragma once
#include "browser/sdk-provider.h"
#include "browser/mission-stepper.h"
#include <functional>

namespace browser {
class MissionInput final : public SdkGraphics {
public:
  struct SimulationSample { uint32_t level; float seconds, deltaSeconds; };
  using SimulationObserver = std::function<void(const SimulationSample&)>;
  using SharedStorage = std::array<std::atomic<uint32_t>, 5>;
  MissionInput(Runtime& runtime, PreparedImage& image, std::shared_ptr<SdkGraphics> controller, std::shared_ptr<SharedStorage> storage, SimulationObserver observer = {});
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const override;
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override;
  struct StepSnapshot { uint64_t steps, skippedDraws, updateMicroseconds, renderMicroseconds, backlogTicks; };
  StepSnapshot SnapshotSteps() const;
private:
  void ObserveSimulation(const X86Context& context);
  struct Scene { uint32_t level = 0, viewport = 0; float seconds = 0; bool active = false; };
  Scene ReadScene(const X86Context& context);
  void Tick(X86Context& context, uint8_t* base);
  void Draw(X86Context& context, uint8_t* base);
  MissionStepper stepper_;
  uint32_t previousLevel_ = 0;
  float previousSeconds_ = 0;
  std::atomic<uint64_t> steps_ = 0, skippedDraws_ = 0, updateMicroseconds_ = 0, renderMicroseconds_ = 0, backlogTicks_ = 0;
  static thread_local MissionInput* stepping_;
  static thread_local MissionInput* suppressed_;
  static thread_local uint32_t viewport_;
  SimulationObserver observer_;
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* memory_;
  std::shared_ptr<SdkGraphics> controller_;
  std::shared_ptr<SharedStorage> storage_;
};
}
