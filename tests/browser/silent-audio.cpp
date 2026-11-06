#include "browser/silent-audio.h"
#include "browser/timer-provider.h"
#include <array>
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

const auto refuse = [](const auto& operation) {
  bool refused = false;
  try { operation(); } catch (const browser::Fault&) { refused = true; }
  check(refused, "invalid silent audio operation succeeded");
};

struct Fixture {
  std::atomic<uint64_t> source = 0;
  browser::Runtime runtime{{}, {}, [&] { return source.load(); }};
  browser::PreparedImage image;
  std::shared_ptr<browser::TimerQueue> queue;
  std::shared_ptr<browser::SilentAudioSink> sink;
  explicit Fixture(std::function<void(const X86Context&, uint64_t)> beforeAdvance = {}, std::shared_ptr<browser::AudioOutput> output = {}) {
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    queue = browser::InstallTimerProvider(runtime, image);
    const auto tick = browser::InstallTimerTickCount(runtime, image, queue);
    sink = std::make_shared<browser::SilentAudioSink>(runtime, image, std::move(output));
    browser::InstallTimerClockObserver(runtime, image, queue,
      [sink = sink, beforeAdvance = std::move(beforeAdvance)](const X86Context& context, uint64_t now) {
        if (beforeAdvance) { beforeAdvance(context, now); }
        sink->Advance(context, now);
      });
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 2> imports = {0x80000071, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 113}};
    browser::BindImageImports(image, runtime, std::span(&tick, 1));
  }
  void Run(const std::function<void(X86Context&, uint8_t*)>& operation) {
    const auto thread = browser::PrepareImageThread(image, runtime, uint32_t((image.threads.size() + 1) * 4), 0x10000);
    runtime.Launch(thread.context, image.memory->Base(), operation);
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
};

void arithmetic() {
  Fixture fixture;
  auto& sink = *fixture.sink;
  auto& runtime = fixture.runtime;
  std::vector<float> samples(browser::SilentAudioSink::kCapacityFrames * audio::kChannels, 0.25f);
  fixture.Run([&](X86Context& context, uint8_t* base) {
    const auto drive = [&](uint64_t now) { fixture.source = now; fixture.queue->Drive(context, base); };
    const auto idleSequence = runtime.Sequence();
    drive(1000000000);
    check(runtime.Sequence() == idleSequence, "idle clock observer fabricated runtime wake");
    check(sink.Snapshot().consumedFrames == 0 && sink.Snapshot().submittedFrames == 0,
      "idle sink fabricated submitted or consumed frames");
    sink.Submit(context, std::span(samples).first(6));
    drive(1000000001);
    check(sink.Snapshot().queuedFrames == 1, "long idle credited future audio");
    drive(1000000208);
    check(sink.Snapshot().consumedFrames == 0 && runtime.Sequence() == idleSequence,
      "fractional frame consumed early or fabricated a wake");
    drive(1000000209);
    check(sink.Snapshot().consumedFrames == 1 && runtime.Sequence() == idleSequence + 1,
      "fractional frame failed to consume or wake runtime");
    sink.Submit(context, std::span(samples).first(18));
    drive(1000000417);
    check(sink.Snapshot().consumedFrames == 1, "drained queue retained fractional credit");
    drive(1000000418);
    check(sink.Snapshot().consumedFrames == 2, "fractional first frame missing");
    drive(1000000626);
    check(sink.Snapshot().consumedFrames == 3, "fractional remainder was discarded");
    sink.Pause(context, true);
    const auto paused = sink.Snapshot();
    drive(2000000626);
    check(sink.Snapshot().consumedFrames == paused.consumedFrames && sink.Snapshot().paused,
      "paused time consumed audio");
    sink.Pause(context, false);
    drive(2000000833);
    check(sink.Snapshot().queuedFrames == 1, "resume credited paused time");
    drive(2000000834);
    check(sink.Snapshot().queuedFrames == 0, "resume lost fractional position");
    const auto beforeInvalid = sink.Snapshot();
    refuse([&] { sink.Submit(context, std::span(samples).first(7)); });
    refuse([&] { sink.Submit(context, std::span(samples).first(1)); });
    check(sink.Snapshot() == beforeInvalid, "invalid sample span mutated accounting");
    sink.Submit(context, samples);
    const auto full = sink.Snapshot();
    refuse([&] { sink.Submit(context, std::span(samples).first(6)); });
    check(sink.Snapshot() == full, "capacity failure was not atomic");
    drive(UINT64_MAX);
    const auto drained = sink.Snapshot();
    check(drained.queuedFrames == 0 && drained.submittedFrames == drained.consumedFrames,
      "large elapsed time overflowed or consumed unsubmitted frames");
    sink.Submit(context, {});
    check(sink.Snapshot() == drained, "empty submit fabricated frames");
    sink.Submit(context, std::span(samples).first(6));
    drive(UINT64_MAX);
    check(sink.Snapshot().queuedFrames == 1, "maximum time wrapped into free credit");
  });
  const auto snapshot = sink.Snapshot();
  check(snapshot.submittedFrames == uint64_t(browser::SilentAudioSink::kCapacityFrames) + 5 &&
    snapshot.consumedFrames + snapshot.queuedFrames == snapshot.submittedFrames,
    "postjoin snapshot lost coherent 64 bit accounting");
}

void submitAfterIdle() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    std::vector<float> samples((browser::SilentAudioSink::kCapacityFrames + 1) * audio::kChannels, 0.5f);
    fixture.sink->Submit(context, std::span(samples).first(12));
    fixture.source = 1000000000;
    const auto beforeFailure = fixture.sink->Snapshot();
    refuse([&] { fixture.sink->Submit(context, samples); });
    check(fixture.sink->Snapshot() == beforeFailure, "late rejected submit changed pending counters");
    fixture.sink->Submit(context, std::span(samples).first(18));
    const auto appended = fixture.sink->Snapshot();
    check(appended.submittedFrames == 5 && appended.consumedFrames == 2 && appended.queuedFrames == 3,
      "late submission inherited an old queue's elapsed idle credit");
    fixture.queue->Drive(context, base);
    check(fixture.sink->Snapshot() == appended, "late submission consumed new frames at its submit timestamp");
    fixture.source += 208;
    fixture.queue->Drive(context, base);
    check(fixture.sink->Snapshot().consumedFrames == 2, "late submission inherited a drained fractional remainder");
    fixture.source += 1;
    fixture.queue->Drive(context, base);
    check(fixture.sink->Snapshot().consumedFrames == 3, "late submission lost its own clock origin");
  });
}

void ownership() {
  browser::Runtime runtime({}, {}), foreign({}, {});
  browser::PreparedImage missing;
  refuse([&] { browser::SilentAudioSink sink(runtime, missing); });
  Fixture fixture;
  X86Context unowned{};
  unowned.host = &fixture.runtime;
  refuse([&] { fixture.sink->Submit(unowned, {}); });
  refuse([&] { fixture.sink->Advance(unowned, 0); });
  refuse([&] { fixture.sink->Pause(unowned, true); });
  refuse([&] { browser::SilentAudioSink sink(fixture.runtime, fixture.image); });
  const auto observer = [](const X86Context&, uint64_t) {};
  refuse([&] { browser::InstallTimerClockObserver(fixture.runtime, fixture.image, fixture.queue, observer); });
  fixture.Run([&](X86Context& context, uint8_t*) {
    auto alien = context;
    alien.host = &foreign;
    refuse([&] { fixture.sink->Submit(alien, {}); });
    refuse([&] { fixture.sink->Advance(alien, 0); });
    refuse([&] { fixture.sink->Pause(alien, true); });
    alien = context;
    alien.fs_base += 4;
    refuse([&] { fixture.sink->Submit(alien, {}); });
    {
      auto memory = std::move(fixture.image.memory);
      refuse([&] { fixture.sink->Submit(context, {}); });
      refuse([&] { fixture.sink->Advance(context, 0); });
      fixture.image.memory = std::move(memory);
    }
    refuse([&] { browser::SilentAudioSink sink(fixture.runtime, fixture.image); });
    refuse([&] { browser::InstallTimerClockObserver(fixture.runtime, fixture.image, fixture.queue, observer); });
    {
      auto lease = fixture.image.memory->Borrow();
      refuse([&] { fixture.sink->Submit(context, {}); });
      refuse([&] { fixture.sink->Advance(context, 0); });
      refuse([&] { fixture.sink->Pause(context, true); });
    }
    fixture.runtime.Cancel();
    refuse([&] { fixture.sink->Submit(context, {}); });
    refuse([&] { fixture.sink->Advance(context, 0); });
    refuse([&] { fixture.sink->Pause(context, true); });
  });
  browser::PreparedImage stopped;
  stopped.memory = std::make_unique<browser::Memory>();
  runtime.Cancel();
  refuse([&] { browser::SilentAudioSink sink(runtime, stopped); });
}

void observerGates() {
  const auto observer = [](const X86Context&, uint64_t) {};
  browser::Runtime runtime({}, {}), foreign({}, {});
  browser::PreparedImage image, other;
  image.memory = std::make_unique<browser::Memory>();
  other.memory = std::make_unique<browser::Memory>();
  const auto queue = browser::InstallTimerProvider(runtime, image);
  refuse([&] { browser::InstallTimerClockObserver(runtime, image, queue, observer); });
  browser::InstallTimerExpiryDriver(runtime, image, queue);
  image.diagnosticRuntime = &foreign;
  refuse([&] { browser::SilentAudioSink sink(runtime, image); });
  refuse([&] { browser::InstallTimerClockObserver(runtime, image, queue, observer); });
  image.diagnosticRuntime = nullptr;
  X86Context context{};
  context.host = &runtime;
  runtime.Launch(context, image.memory->Base(), [&](X86Context&, uint8_t*) {
    refuse([&] { browser::SilentAudioSink sink(runtime, image); });
    refuse([&] { browser::InstallTimerClockObserver(runtime, image, queue, observer); });
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  refuse([&] { browser::InstallTimerClockObserver(foreign, image, queue, observer); });
  refuse([&] { browser::InstallTimerClockObserver(runtime, other, queue, observer); });
  refuse([&] { browser::InstallTimerClockObserver(runtime, image, nullptr, observer); });
  refuse([&] { browser::InstallTimerClockObserver(runtime, image, queue, {}); });
  {
    auto lease = image.memory->Borrow();
    refuse([&] { browser::InstallTimerClockObserver(runtime, image, queue, observer); });
  }
  browser::InstallTimerClockObserver(runtime, image, queue, observer);
  refuse([&] { browser::InstallTimerClockObserver(runtime, image, queue, observer); });
  browser::Runtime cancelled({}, {});
  browser::PreparedImage cancelledImage;
  cancelledImage.memory = std::make_unique<browser::Memory>();
  const auto cancelledQueue = browser::InstallTimerProvider(cancelled, cancelledImage);
  browser::InstallTimerExpiryDriver(cancelled, cancelledImage, cancelledQueue);
  cancelled.Cancel();
  refuse([&] { browser::InstallTimerClockObserver(cancelled, cancelledImage, cancelledQueue, observer); });
}

void concurrent() {
  Fixture fixture;
  std::atomic<bool> start = false;
  std::atomic<bool> finished = false;
  std::atomic<uint64_t> publications = 0;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    fixture.runtime.Launch(context, base, [&](X86Context& child, uint8_t* childBase) {
      while (!start.load()) { std::this_thread::yield(); }
      for (uint32_t n = 0; n < 20000; ++n) {
        fixture.queue->Drive(child, childBase);
        const auto snapshot = fixture.sink->Snapshot();
        check(snapshot.submittedFrames == snapshot.consumedFrames + snapshot.queuedFrames &&
          snapshot.queuedFrames <= browser::SilentAudioSink::kCapacityFrames, "concurrent snapshot tore counters");
        ++publications;
      }
      finished = true;
    });
    start = true;
    const std::array<float, 6> frame = {1, 2, 3, 4, 5, 6};
    for (uint32_t n = 0; n < 20000; ++n) {
      fixture.sink->Submit(context, frame);
      fixture.source += 209;
      fixture.queue->Drive(context, base);
    }
    while (!finished.load()) { std::this_thread::yield(); }
    fixture.source += 10000000;
    fixture.queue->Drive(context, base);
  });
  const auto snapshot = fixture.sink->Snapshot();
  check(snapshot.submittedFrames == 20000 && snapshot.consumedFrames == 20000 && snapshot.queuedFrames == 0 &&
    publications == 20000, "concurrent submission lost or fabricated frames");
}

void submitDuringClockPublication() {
  std::atomic<bool> sampled = false;
  std::atomic<bool> release = false;
  std::atomic<bool> delivered = false;
  Fixture fixture([&](const X86Context&, uint64_t now) {
    if (!sampled.exchange(true)) {
      check(now == 100, "race fixture did not capture the earlier owned clock sample");
      while (!release.load()) { std::this_thread::yield(); }
    }
  });
  fixture.Run([&](X86Context& context, uint8_t* base) {
    fixture.source = 100;
    fixture.runtime.Launch(context, base, [&](X86Context& child, uint8_t* childBase) {
      fixture.queue->Drive(child, childBase);
      delivered = true;
    });
    while (!sampled.load()) { std::this_thread::yield(); }
    fixture.source = 200;
    const std::array<float, 6> frame = {1, 2, 3, 4, 5, 6};
    fixture.sink->Submit(context, frame);
    release = true;
    while (!delivered.load()) { std::this_thread::yield(); }
    fixture.queue->Drive(context, base);
    fixture.source = 408;
    fixture.queue->Drive(context, base);
    check(fixture.sink->Snapshot().consumedFrames == 0,
      "stale real observer delivery rolled back the later submission origin");
    fixture.source = 409;
    fixture.queue->Drive(context, base);
    check(fixture.sink->Snapshot().consumedFrames == 1, "stale observer race lost submitted audio");
  });
}

void serializedPublication() {
  browser::Runtime runtime({}, {}, [] {
    static std::atomic<uint64_t> sample = 0;
    return sample.fetch_add(1);
  });
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 4096);
  const auto queue = browser::InstallTimerProvider(runtime, image);
  browser::InstallTimerExpiryDriver(runtime, image, queue);
  uint64_t last = 0;
  uint64_t calls = 0;
  browser::InstallTimerClockObserver(runtime, image, queue, [&](const X86Context& context, uint64_t now) {
    check(!browser::Memory::HasBorrow() && runtime.OwnsCurrentContext(context), "observer inherited lease or foreign context");
    std::this_thread::yield();
    check(now >= last, "two workers published reversed clock samples");
    last = now;
    ++calls;
  });
  image.image.kernel_thunk = 0x10000;
  const uint32_t zero = 0;
  image.memory->Write({}, 0x10000, &zero, 4);
  browser::BindImageImports(image, runtime, {});
  const auto thread = browser::PrepareImageThread(image, runtime, 4, 0x10000);
  runtime.Launch(thread.context, image.memory->Base(), [&](X86Context& context, uint8_t* base) {
    runtime.Launch(context, base, [&](X86Context& child, uint8_t* childBase) {
      for (uint32_t n = 0; n < 10000; ++n) { queue->Drive(child, childBase); }
    });
    const uint8_t irql = 2;
    image.memory->Write(context, context.fs_base + 0x24, &irql, 1);
    std::unique_lock delivery(image.dpcDeliveryLock);
    const auto progress = runtime.ProgressAddress()->load();
    for (uint32_t n = 0; n < 10000; ++n) { queue->Drive(context, base); }
    check(runtime.ProgressAddress()->load() == progress, "observer manufactured guest progress");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(calls == 20000 && queue->Pending() == 0, "observer required pending timers or deferred delivery");
}

void outputRing() {
  const auto owned = std::make_unique<browser::AudioOutput>();
  auto& output = *owned;
  std::vector<float> samples(browser::AudioOutput::kFrames * audio::kChannels, 0.25f);
  check(!output.Submit(samples), "disabled browser output accepted samples");
  output.header[3] = 1;
  check(output.Submit(samples), "bounded browser output rejected a full ring");
  check(output.header[0] == browser::AudioOutput::kFrames && output.samples[0] == 0.25f,
    "browser output did not publish interleaved samples");
  check(!output.Submit(std::span(samples).first(6)), "browser output overwrote unread samples");
  output.header[1] = browser::AudioOutput::kFrames;
  check(output.Submit(std::span(samples).first(6)), "browser output failed to wrap");
  check(output.header[0] == browser::AudioOutput::kFrames + 1 && output.samples[0] == 0.25f,
    "browser output published the wrong wrap position");
  const uint32_t epoch = output.header[2];
  output.Reset();
  check(!output.header[3] && output.header[2] == epoch + 1, "runtime reset left browser audio enabled or stale");
  output.header[0] = UINT32_MAX;
  output.header[1] = UINT32_MAX;
  output.header[3] = 1;
  check(output.Submit(std::span(samples).first(12)) && output.header[0] == 1,
    "browser output counters failed to wrap at uint32");
}

void outputCushion() {
  const auto output = std::make_shared<browser::AudioOutput>();
  Fixture fixture({}, output);
  output->header[3] = 1;
  const auto frames = [](uint32_t first, uint32_t count) {
    std::vector<float> samples(size_t(count) * audio::kChannels);
    for (uint32_t n = 0; n < count; ++n) { samples[size_t(n) * audio::kChannels] = float(first + n); }
    return samples;
  };
  const auto queued = [&] { return output->header[0].load() - output->header[1].load(); };
  const auto first = [&] { return output->samples[size_t(output->header[1].load() % browser::AudioOutput::kFrames) * audio::kChannels]; };
  fixture.Run([&](X86Context& context, uint8_t*) {
    fixture.sink->SubmitAtTime(context, 0, frames(0, 8192));
    check(queued() == 8192, "the first submission did not fill the browser ring");
    output->header[1] += 2400;
    output->header[1] = output->header[0].load();
    fixture.sink->SubmitAtTime(context, 500000, frames(8192, 2400));
    check(queued() == 8192, "a flushed browser ring did not get the frames the guest has not played yet");
    check(first() == 2400, "the refilled browser ring does not start where the guest clock is");
    output->header[1] += 5792;
    check(first() == 8192, "the refilled browser ring is not followed by the new frames");
    output->header[1] = output->header[0].load();
    fixture.sink->SubmitAtTime(context, 500000 + 1700000, frames(10592, 2400));
    check(queued() == 2400 && first() == 10592, "a browser ring drained with the guest clock replayed old frames");
  });
}

int main() {
  outputRing();
  outputCushion();
  arithmetic();
  submitAfterIdle();
  ownership();
  observerGates();
  concurrent();
  submitDuringClockPublication();
  serializedPublication();
  std::puts("silent-audio-contracts-ok capacity-frames=48000 rate=48000 channels=6 clock-unit-ns=100");
}
