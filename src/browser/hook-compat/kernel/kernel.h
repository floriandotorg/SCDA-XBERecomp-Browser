#pragma once
// Browser stand-in for src/kernel/kernel.h, first on the include path of native game hooks: the native helper names
// over the bound session's browser::Memory instead of one flat host mapping.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"

#if !defined(X86_COMPACT_MEMORY)
#error "native game hooks build only against browser memory"
#endif

#include "browser/hooks.h"

namespace browser::hooks {
template <typename T>
inline T Load(uint32_t va) {
  T value;
  Bound().Read(NoContext(), va, &value, sizeof(T));
  return value;
}
template <typename T>
inline void Store(uint32_t va, T value) {
  Bound().Write(NoContext(), va, &value, sizeof(T));
}
}

namespace kernel {

enum : uint32_t {
  kSuccess = 0x00000000,
  kWait0 = 0x00000000,
  kAbandoned = 0x00000080,
  kUserApc = 0x000000C0,
  kAlerted = 0x00000101,
  kTimeout = 0x00000102,
  kPending = 0x00000103,
  kObjectNameExists = 0x40000000,
  kNoMoreFiles = 0x80000006,
  kUnsuccessful = 0xC0000001,
  kNotImplemented = 0xC0000002,
  kInvalidHandle = 0xC0000008,
  kInvalidParameter = 0xC000000D,
  kNoSuchFile = 0xC000000F,
  kEndOfFile = 0xC0000011,
  kNoMemory = 0xC0000017,
  kAccessDenied = 0xC0000022,
  kBufferTooSmall = 0xC0000023,
  kObjectTypeMismatch = 0xC0000024,
  kObjectNameInvalid = 0xC0000033,
  kObjectNameNotFound = 0xC0000034,
  kObjectNameCollision = 0xC0000035,
  kObjectPathNotFound = 0xC000003A,
  kMutantNotOwned = 0xC0000046,
  kSemaphoreLimitExceeded = 0xC0000047,
  kNotADirectory = 0xC0000103,
  kFileIsADirectory = 0xC00000BA,
  kNotSupported = 0xC00000BB,
  kDeviceNotConnected = 0xC000009D,
  kCannotDelete = 0xC0000121,
};

// valid while the calling guest worker keeps its backing lease: not across a CallGuest that may wait
template <typename T>
inline T& G(uint32_t va) {
  return *reinterpret_cast<T*>(browser::hooks::Bound().Span(browser::hooks::NoContext(), va, sizeof(T)));
}
inline uint8_t R8(uint32_t va) { return browser::hooks::Load<uint8_t>(va); }
inline uint16_t R16(uint32_t va) { return browser::hooks::Load<uint16_t>(va); }
inline uint32_t R32(uint32_t va) { return browser::hooks::Load<uint32_t>(va); }
inline uint64_t R64(uint32_t va) { return browser::hooks::Load<uint64_t>(va); }
inline void W16(uint32_t va, uint16_t v) { browser::hooks::Store(va, v); }
inline void W32(uint32_t va, uint32_t v) { browser::hooks::Store(va, v); }
inline void W64(uint32_t va, uint64_t v) { browser::hooks::Store(va, v); }
// contiguous to the end of the guest allocation; only its first page is marked written
inline void* HostPtr(uint32_t va) { return browser::hooks::Bound().Span(browser::hooks::NoContext(), va, 1); }

std::string ReadAnsiString(uint32_t str);
std::string ReadCString(uint32_t va, size_t max = 4096);

struct Args {
  X86Context& ctx;
  uint32_t operator[](int i) const { return R32(ctx.esp + 4 + 4 * uint32_t(i)); }
};
inline void Return(X86Context& ctx, uint32_t value, int stack_args) {
  ctx.eax = value;
  ctx.esp += 4 + 4 * uint32_t(stack_args);
}

inline uint32_t CallGuest(X86Context& ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
  return browser::Runtime::From(ctx).Call(ctx, browser::hooks::Base(), fn, args);
}

}  // namespace kernel
