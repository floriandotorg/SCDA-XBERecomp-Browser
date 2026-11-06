#include "browser/hook-compat/kernel/kernel.h"
#include <atomic>

namespace browser::hooks {
namespace {
std::atomic<uint8_t*> bound = nullptr;
std::atomic<uint64_t> generation = 0;
const X86Context noContext{};
}

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
