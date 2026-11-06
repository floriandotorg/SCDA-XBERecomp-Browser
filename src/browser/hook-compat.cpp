#include "browser/hook-compat/kernel/kernel.h"
#include <atomic>
#include <mutex>

namespace browser::hooks {
namespace {
std::atomic<uint8_t*> bound = nullptr;
std::atomic<uint64_t> generation = 0;
const X86Context noContext{};
std::mutex poolLock;
uint32_t pool = 0, poolUsed = 0;
std::atomic<Runtime*> runtime = nullptr;
}

void BindPool(uint32_t address) {
  std::lock_guard lock(poolLock);
  if (address != pool) { poolUsed = 0; }
  pool = address;
}

uint32_t PoolUsed() {
  std::lock_guard lock(poolLock);
  return poolUsed;
}

void BindRuntime(Runtime* value) { runtime.store(value, std::memory_order_release); }

void Bind(Memory& memory) {
  bound.store(memory.Base(), std::memory_order_release);
  generation.fetch_add(1, std::memory_order_acq_rel);
}

void Unbind() { bound.store(nullptr, std::memory_order_release); }

uint64_t Generation() { return generation.load(std::memory_order_acquire); }

uint8_t* Base() {
  auto* base = bound.load(std::memory_order_acquire);
  if (!base) { throw Fault(noContext, 0, 0, "game hook ran without a bound guest memory"); }
  return base;
}

Memory& Bound() { return Memory::From(Base()); }

const X86Context& NoContext() { return noContext; }

}

namespace kernel {

std::string ReadAnsiString(uint32_t str) {
  const uint16_t length = R16(str);
  const uint32_t buffer = R32(str + 4);
  std::string text(length, '\0');
  if (length) { browser::hooks::Bound().Read(browser::hooks::NoContext(), buffer, text.data(), length); }
  return text;
}

std::string ReadCString(uint32_t va, size_t max) {
  std::string text;
  for (char c; text.size() < max && (c = char(R8(va))); ++va) { text += c; }
  return text;
}

}

namespace mem {

bool IsCommitted(uint32_t va) { return browser::hooks::Bound().IsCommitted(va); }

uint32_t AllocContiguous(uint32_t size, uint32_t, uint32_t, uint32_t align) {
  namespace hooks = browser::hooks;
  std::lock_guard lock(hooks::poolLock);
  align = align < 16 ? 16 : align;
  const uint32_t start = (hooks::poolUsed + align - 1) & ~(align - 1);
  if (!hooks::pool || !size || start > hooks::kPoolBytes || size > hooks::kPoolBytes - start) {
    LOGW("hooks: no room for %u bytes in the hook pool", size);
    return 0;
  }
  hooks::poolUsed = start + size;
  return hooks::pool + start;
}

uint32_t AllocSystem(uint32_t size, uint32_t align) { return AllocContiguous(size, 0, 0xFFFFFFFF, align); }

}

namespace dispatch {

uint32_t HostThunk(X86Func* fn) {
  auto* runtime = browser::hooks::runtime.load(std::memory_order_acquire);
  if (!runtime) { throw browser::Fault(browser::hooks::NoContext(), 0, 0, "host thunk without a bound runtime"); }
  return runtime->HostThunk(fn);
}

}
