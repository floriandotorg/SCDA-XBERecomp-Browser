#include "browser/thread-provider.h"
#include <algorithm>
#include <array>

namespace browser {
namespace {

struct ThreadExit { uint32_t status; };

class ThreadProvider {
  Runtime& runtime_;
  PreparedImage& image_;
  std::function<bool(uint32_t)> closeFile_;
  std::mutex lock_;
  struct Thread {
    std::mutex publication;
    uint32_t handle = 0x70000004;
    uint32_t kpcr = 0;
    bool closed = false;
    std::atomic<bool> completed = false;
    uint32_t exitStatus = 0;
  };
  std::shared_ptr<Thread> thread_;

  void RequireOwner(const X86Context& ctx, uint8_t* base) {
    if (ctx.host != &runtime_ || !image_.importsBound || image_.boundRuntime != &runtime_ ||
        base != image_.memory->Base() || !runtime_.OwnsCurrentWorker()) {
      throw Fault(ctx, ctx.esp, 0, "thread service requires its owned image worker");
    }
  }

public:
  ThreadProvider(Runtime& runtime, PreparedImage& image, std::function<bool(uint32_t)> closeFile)
    : runtime_(runtime), image_(image), closeFile_(std::move(closeFile)) {}

  void Create(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "thread creation during backing lease"); }
    std::array<uint32_t, 11> frame;
    auto& memory = *image_.memory;
    if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "thread frame overflow"); }
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    const uint32_t output = frame[1], extension = frame[2], stack = frame[3], tls = frame[4], idOutput = frame[5];
    const uint32_t start = frame[6], argument = frame[7], suspended = frame[8], debugger = frame[9], system = frame[10];
    const uint32_t expectedTls = image_.image.tls_addr ? ((uint32_t(image_.tlsTemplate.size()) + 15) & ~15u) + 4 : 0;
    if (!output || extension || suspended || debugger || tls != expectedTls || !runtime_.Find(start) ||
        (system && !runtime_.Find(system)) || !image_.tlsCallbacks.empty()) {
      throw Fault(ctx, 255, 0, "unsupported system thread creation arguments");
    }
    if (idOutput && uint64_t(output) < uint64_t(idOutput) + 4 && uint64_t(idOutput) < uint64_t(output) + 4) {
      throw Fault(ctx, output, 4, "thread outputs overlap");
    }
    memory.CheckRange(ctx, output, 4);
    if (idOutput) { memory.CheckRange(ctx, idOutput, 4); }
    std::lock_guard lock(lock_);
    if (thread_) { throw Fault(ctx, 255, 0, "session thread storage capacity exhausted"); }
    auto state = std::make_shared<Thread>();
    std::unique_lock publication(state->publication);
    LaunchImageThread(image_, runtime_, ctx, 0x14, stack, [&](const ImageThread& thread) {
      auto lease = memory.Borrow();
      auto* handleBytes = memory.Span(ctx, output, 4);
      auto* idBytes = idOutput ? memory.Span(ctx, idOutput, 4) : nullptr;
      state->kpcr = thread.kpcr;
      X86Context child = thread.context;
      child.thread_handle = state->handle;
      runtime_.Launch(child, base, [this, state, start, argument, system](X86Context& child, uint8_t* backing) {
        { std::lock_guard ready(state->publication); }
        runtime_.Checkpoint(child);
        try {
          state->exitStatus = system ? runtime_.Call(child, backing, system, {start, argument})
                                    : runtime_.Call(child, backing, start, {argument});
          runtime_.Checkpoint(child);
          state->completed = true;
        } catch (const ThreadExit& exit) {
          runtime_.Checkpoint(child);
          state->exitStatus = exit.status;
          state->completed = true;
        }
      });
      std::memcpy(handleBytes, &state->handle, 4);
      if (idBytes) { std::memcpy(idBytes, &thread.id, 4); }
      thread_ = state;
      ctx.eax = 0;
      ctx.esp += sizeof(frame);
    });
  }

  void Close(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    std::array<uint32_t, 2> frame;
    if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "close frame overflow"); }
    image_.memory->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    std::lock_guard lock(lock_);
    const bool live = thread_ && !thread_->closed && thread_->handle == frame[1];
    if (live) { thread_->closed = true; }
    ctx.eax = live || (closeFile_ && closeFile_(frame[1])) ? 0 : 0xc0000008;
    ctx.esp += sizeof(frame);
  }

  void Terminate(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    std::array<uint32_t, 2> frame;
    image_.memory->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    std::lock_guard lock(lock_);
    if (!thread_ || thread_->kpcr != ctx.fs_base) { throw Fault(ctx, 258, 0, "termination requires a managed child thread"); }
    throw ThreadExit{frame[1]};
  }
};

}

void InstallThreadProvider(Runtime& runtime, PreparedImage& image, std::function<bool(uint32_t)> closeFile) {
  auto provider = std::make_shared<ThreadProvider>(runtime, image, std::move(closeFile));
  runtime.RegisterKernel(255, [provider](X86Context& ctx, uint8_t* base) { provider->Create(ctx, base); });
  runtime.RegisterKernel(187, [provider](X86Context& ctx, uint8_t* base) { provider->Close(ctx, base); });
  runtime.RegisterKernel(258, [provider](X86Context& ctx, uint8_t* base) { provider->Terminate(ctx, base); });
}

}
