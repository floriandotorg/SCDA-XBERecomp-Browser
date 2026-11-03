#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"
#ifdef BROWSER_FULL_METADATA
#include "build_identity.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cfenv>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

using browser::Memory;
using browser::Runtime;

const auto check = [](bool valid, const char* operation) {
  if (!valid) { throw std::runtime_error(operation); }
};
const auto fault = [](auto operation, const char* name) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, name);
};

X86_EXTERN_FUNC(sub_00011000);
X86_EXTERN_FUNC(sub_003E0540);
X86_EXTERN_FUNC(sub_0019B695);
X86_EXTERN_FUNC(sub_0016E504);
X86_EXTERN_FUNC(sub_00011013);
X86_EXTERN_FUNC(__imp__sub_0016E504);
X86_EXTERN_FUNC(sub_002CDA10);
X86_EXTERN_FUNC(sub_00452604);
X86_EXTERN_FUNC(sub_00452449);
X86_EXTERN_FUNC(sub_00365CA1);
X86_EXTERN_FUNC(sub_00309660);

X86_FUNC(sub_0016E504) { __imp__sub_0016E504(ctx, base); ++ctx.eax; }
X86_EXTERN_FUNC(__imp__sub_002CDA25);
uint32_t forcedResumeStack = 0;
bool nestedExceptionTest = false;
uint32_t innerReturns = 0;
X86_FUNC(sub_002CD410) {
  if (forcedResumeStack) { ctx.esp = forcedResumeStack; }
  x86_icall(ctx, base, 0x2100);
}
#ifndef BROWSER_FULL_METADATA
X86_FUNC(sub_002CD860) { x86_trap(ctx, 0x002CD860, "unexercised exception fixture dependency"); }
#endif
X86_FUNC(sub_002CDA25) {
  if (nestedExceptionTest) {
    __imp__sub_002CDA25(ctx, base);
    ++innerReturns;
    return;
  }
  sub_002CD410(ctx, base);
}

X86_FUNC(callback) { ctx.eax = 0x12345678; ctx.ecx = 99; ctx.esp += 4; }
X86_FUNC(continuation) { ctx.eax += 10; ctx.esp += 4; }
X86_FUNC(propagate) { x86_icall(ctx, base, 0x2100); }
std::atomic<uint32_t> workersEntered = 0;
X86_FUNC(waitingWorker) {
  auto& runtime = Runtime::From(ctx);
  check(x86_rdtsc() == runtime.ticks.load(), "owned worker has its own runtime clock binding");
  ++workersEntered;
  runtime.Wait(ctx, runtime.Sequence(), std::chrono::seconds(10));
}
X86_FUNC(completingWorker) { X86_MEM32(0x80000600) = ctx.thread_handle; }
X86_FUNC(joiningWorker) { Runtime::From(ctx).Join(); }
X86_FUNC(failingWorker) { x86_trap(ctx, 0x1234, "synthetic guest worker fault"); }
X86_FUNC(nested) {
  auto& runtime = Runtime::From(ctx);
  ctx.eax = runtime.Call(ctx, base, 0x2000, {7, 8});
  ctx.esp += 4;
}

int main() {
  try {
    std::printf("cpu-work-begin\n");
    Memory memory;
    memory.Map(0x10000, 0x2000);
    memory.Map(0x12000, 0x2000);
    memory.Map(0xD0000000, 0x10000);
    memory.Map(0x5E3000, 0x1000);
    memory.Map(0x20000, 0x20000);
    memory.Commit(0x10000, 0x4000);
    memory.Commit(0xD0001000, 0xF000);
    memory.Commit(0x5E3000, 0x1000);
    memory.Commit(0x20000, 0x20000);
    X86Context ctx{};
    ctx.esp = 0xD000F000;
    ctx.last_icall = 0xABCDEF;
    const std::string diagnosticReason(512, 'x');
    const browser::Fault diagnosticFault(ctx, 0x12345678, 4096, diagnosticReason.c_str());
    const std::string diagnostic = diagnosticFault.what();
    check(diagnostic.starts_with(diagnosticReason), "bounded resource diagnostic retains its complete reason");
    check(diagnostic.ends_with("last_icall=0x00ABCDEF"), "resource diagnostic retains guest context");
    const std::string oversizedReason(4096, 'y');
    const browser::Fault oversizedFault(ctx, 0, 0, oversizedReason.c_str());
    check(std::string(oversizedFault.what()).size() <= 2047, "oversized fault diagnostic stays bounded");
    uint8_t* base = memory.Base();
    X86_MEM32(0x80000001) = 0x78563412;
    check(uint32_t(X86_MEM32(0xF0000001)) == 0x78563412, "physical alias unaligned dword");
    X86_MEM64(0x11FFD) = 0x8877665544332211ull;
    check(uint64_t(X86_MEM64(0x11FFD)) == 0x8877665544332211ull, "split region qword");
    X86_MEM32(0x10001) = X86_MEM32(0xF0000001);
    check(uint32_t(X86_MEM32(0x10001)) == 0x78563412, "proxy to proxy movsd");
    fault([&] { memory.Span(ctx, 0x11FFD, 8); }, "host span must not cross storage");
    fault([&] { auto value = uint32_t(X86_MEM32(0xFFFFFFFE)); (void)value; }, "address wrap");
    fault([&] { X86_MEM32(0x13FFF) = 1; }, "unmapped crossing store");
    memory.Decommit(0x12000, 0x1000);
    fault([&] { auto value = uint8_t(X86_MEM8(0x12000)); (void)value; }, "decommitted read");
    memory.Commit(0x12000, 0x1000);
    check(uint32_t(X86_MEM32(0x12000)) == 0, "recommit zero");
    X86_MEM8(0x80000100) = 3;
    ctx.esi = 0x80000100; ctx.edi = 0xF0000101; ctx.ecx = 7;
    x86_rep_movs(ctx, base, 1);
    check(uint64_t(X86_MEM64(0x80000100)) == 0x0303030303030303ull, "rep movsb alias overlap is sequential");
    ctx.esi = 0x12002; ctx.edi = 0x12001; ctx.ecx = 3; ctx.df = 1;
    X86_MEM32(0x12000) = 0x04030201;
    x86_rep_movs(ctx, base, 1);
    check(uint32_t(X86_MEM32(0x12000)) == 0x04030303, "backward rep movsb overlap");
    ctx.df = 0;
    X86_MEM32(0x10000) = 0xCAFEBABE;
    ctx.esi = 0x10000; ctx.edi = 0x13FFC; ctx.ecx = 2;
    fault([&] { x86_rep_movs(ctx, base, 4); }, "forward string fault after one completed element");
    check(ctx.esi == 0x10004 && ctx.edi == 0x14000 && ctx.ecx == 1 && uint32_t(X86_MEM32(0x13FFC)) == 0xCAFEBABE, "forward string fault retains completed bytes and register progress");
    X86_MEM32(0x12004) = 0xDEADBEEF;
    ctx.esi = 0x12004; ctx.edi = 0x10000; ctx.ecx = 2; ctx.df = 1;
    fault([&] { x86_rep_movs(ctx, base, 4); }, "backward string fault after one completed element");
    check(ctx.esi == 0x12000 && ctx.edi == 0xFFFC && ctx.ecx == 1 && uint32_t(X86_MEM32(0x10000)) == 0xDEADBEEF, "backward string fault retains completed bytes and register progress");
    ctx.df = 0;
    memory.Copy(ctx, 0x11000, 0x11FFD, 16);
    check(uint8_t(X86_MEM8(0x11000)) == 0x11, "bulk copy splits storage");
    X86_MEM64(0x80000300) = 0x0807060504030201ull;
    memory.Copy(ctx, 0xF0000301, 0x80000300, 7);
    check(uint64_t(X86_MEM64(0x80000300)) == 0x0706050403020101ull, "bulk memmove alias overlap");
    X86Xmm vector{};
    vector.q[0] = 0x8877665544332211ull;
    vector.q[1] = 0xFFEEDDCCBBAA0099ull;
    *X86_PTR(X86Xmm, 0x11FFD) = vector;
    const X86Xmm loaded = *X86_PTR(X86Xmm, 0x11FFD);
    check(loaded.q[0] == vector.q[0] && loaded.q[1] == vector.q[1], "unaligned split SSE vector");
    {
      auto lease = memory.Borrow();
      const uint8_t extended[] = {0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0x3F};
      memory.Write(ctx, 0x11FFD, extended, sizeof(extended));
      check(x87_load80(X86_PTR(uint8_t, 0x11FFD)) == 1.5, "split x87 ten byte load");
      x87_store80(X86_PTR(uint8_t, 0x11FFD), -1.5);
      check(x87_load80(X86_PTR(uint8_t, 0x11FFD)) == -1.5, "split x87 ten byte store");
      fault([&] { x87_load80(X86_PTR(uint8_t, 0x13FFD)); }, "x87 rejects incomplete ten byte span");
    }
    fault([&] { X86_PTR(uint32_t, 0x80000001); }, "misaligned atomic pointer diagnostic");
    std::vector<std::thread> workers;
    for (uint32_t n = 0; n < 2; ++n) {
      workers.emplace_back([&, n] {
        X86Context ctx{};
        const uint32_t address = n ? 0xF0000200 : 0x80000200;
        for (uint32_t n = 0; n < 10000; ++n) {
          auto lease = memory.Borrow();
          __atomic_fetch_add(X86_PTR(uint32_t, address), 1u, __ATOMIC_SEQ_CST);
        }
      });
    }
    for (auto& worker : workers) { worker.join(); }
    check(uint32_t(X86_MEM32(0x80000200)) == 20000, "workers atomic alias total");
    {
      auto lease = memory.Borrow();
      auto* counter = X86_PTR(uint32_t, 0xF0000200);
      uint32_t expected = 20000;
      check(__atomic_compare_exchange_n(counter, &expected, 7u, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST), "cmpxchg success");
      check(__atomic_exchange_n(counter, 9u, __ATOMIC_SEQ_CST) == 7, "xchg old value");
      const auto atomicWidths = [&]<typename T>(uint32_t address) {
        auto* pointer = X86_PTR(T, address);
        __atomic_store_n(pointer, T(3), __ATOMIC_SEQ_CST);
        check(__atomic_fetch_sub(pointer, T(1), __ATOMIC_SEQ_CST) == 3, "locked sub old value");
        check(__atomic_fetch_or(pointer, T(4), __ATOMIC_SEQ_CST) == 2, "locked or old value");
        check(__atomic_fetch_and(pointer, T(3), __ATOMIC_SEQ_CST) == 6, "locked and old value");
        check(__atomic_fetch_xor(pointer, T(1), __ATOMIC_SEQ_CST) == 2, "locked xor old value");
        T expected = 7;
        check(!__atomic_compare_exchange_n(pointer, &expected, T(9), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) && expected == 3, "cmpxchg failure updates accumulator");
      };
      atomicWidths.operator()<uint8_t>(0x80000210);
      atomicWidths.operator()<uint16_t>(0x80000212);
      atomicWidths.operator()<uint32_t>(0x80000214);
      atomicWidths.operator()<uint64_t>(0x80000218);
    }
    const X86FuncMapping mappings[] = {{0x2300, propagate}, {0x2000, callback}, {0x2100, continuation}, {0x2200, nested}, {0x19B695, sub_0019B695}, {0x3E0540, sub_003E0540}, {0x16E504, sub_0016E504}};
    const uint32_t continuations[] = {0x2100};
#ifdef BROWSER_FULL_METADATA
    std::vector<X86FuncMapping> retained;
    for (size_t n = 0; X86FuncMappings[n].host; ++n) { retained.push_back(X86FuncMappings[n]); }
    check(retained.size() == BROWSER_FUNCTION_COUNT, "complete retained story metadata is linked");
    for (const auto& mapping : mappings) {
      bool found = false;
      for (const auto& original : retained) { found |= original.guest == mapping.guest; }
      if (!found) { retained.push_back(mapping); }
    }
    std::vector<uint32_t> retainedContinuations;
    for (size_t n = 0; X86Continuations[n]; ++n) { retainedContinuations.push_back(X86Continuations[n]); }
    check(!retainedContinuations.empty(), "complete continuation metadata is linked");
    retainedContinuations.insert(retainedContinuations.end(), std::begin(continuations), std::end(continuations));
    Runtime runtime(retained, retainedContinuations);
    std::printf("full-metadata-ok mappings=%zu continuations=%zu\n", retained.size(), retainedContinuations.size());
#else
    Runtime runtime(mappings, continuations);
#endif
    ctx.host = &runtime;
#ifdef BROWSER_FULL_METADATA
    ctx.esp = ctx.ebp = 0xD000F000;
    ctx.esi = 0x10000;
    ctx.eax = 7;
    ctx.ecx = 42;
    X86_MEM32(0x1001E) = 0xAABBCCDD;
    bool misalignedGeneratedAtomic = false;
    try { x86_icall(ctx, base, 0x004DF226); }
    catch (const browser::Fault& error) { misalignedGeneratedAtomic = error.address == 0x1001E && error.size == 4; }
    check(misalignedGeneratedAtomic && uint32_t(X86_MEM32(0x1001E)) == 0xAABBCCDD && ctx.eax == 9 && ctx.ecx == 42 && ctx.esp == 0xD000F004, "004DF229 actual emitted misaligned atomic stops before exchanging bytes");
#endif
    ctx.esp = 0xD000F000;
    ctx.ecx = 0x10001;
    for (uint32_t n = 0; n < 64; ++n) { X86_MEM8(0x10001 + n) = 0xAA; }
    sub_00011000(ctx, base);
    check(ctx.eax == 0x10001 && ctx.ecx == 0 && ctx.esp == 0xD000F004, "00011000 registers and ret");
    for (uint32_t n = 0; n < 64; ++n) {
      const bool cleared = n < 8 || (n >= 0x30 && n < 0x34) || n >= 0x38;
      check(uint8_t(X86_MEM8(0x10001 + n)) == (cleared ? 0 : 0xAA), "00011004..0001100F sparse stores");
    }
    ctx.esp = 0xD000F000; ctx.ecx = 0x10100;
    X86_MEM32(0x10100) = 0x10200; X86_MEM32(0x10240) = 0x2000; X86_MEM32(0x10214) = 0x2000;
    sub_003E0540(ctx, base);
    check(ctx.eax == 0x12345678 && ctx.esp == 0xD000F004, "003E0542 indirect tail call");
    ctx.esp = 0xD000F000; ctx.ecx = 0x10100 - 0x248;
    sub_0019B695(ctx, base);
    check(ctx.eax == 0x12345678, "0019B695 add and indirect call");
    ctx.eax = 17; ctx.ecx = 23; ctx.esp = 0xD000F000;
    check(runtime.Call(ctx, base, 0x2200, {}) == 0x12345678 && ctx.eax == 17 && ctx.ecx == 23 && ctx.esp == 0xD000F000, "nested CallGuest restore");
    try { x86_icall(ctx, base, 0x2100); check(false, "continuation must unwind"); }
    catch (const X86Resume& resume) {
      check(resume.target == 0x2100 && resume.esp == ctx.esp, "continuation payload");
      ctx.esp -= 32;
      x86_resume(ctx, base, resume);
    }
    check(ctx.esp == 0xD000F004 && ctx.eax == 27, "continuation stack restoration");
    const uint32_t host = runtime.HostThunk(callback);
    check(runtime.HostThunk(callback) == host, "host thunk reuse");
    x86_icall(ctx, base, host);
    runtime.RegisterKernel(10, callback);
    x86_icall(ctx, base, 0xFE000028);
    check(ctx.eax == 0x12345678, "host and kernel routes");
    fault([&] { x86_icall(ctx, base, host + 1); }, "unaligned host thunk rejected");
    fault([&] { x86_icall(ctx, base, 0xFE000029); }, "unaligned kernel thunk rejected");
    nestedExceptionTest = true;
    for (uint32_t n = 0; n < 3; ++n) {
      const uint32_t offsets[] = {32, 8, 0};
      ctx.esp = 0xD000F000;
      ctx.fs_base = 0x10000;
      forcedResumeStack = ctx.esp - offsets[n];
      innerReturns = 0;
      bool propagated = false;
      try { sub_002CDA10(ctx, base); }
      catch (const X86Resume&) { propagated = true; }
      check(propagated == (n == 2) && innerReturns == (n == 0 ? 1u : 0u), "002CDA10 and 002CDA25 nested generated catches select the owning guest frame");
      check(ctx.esp == forcedResumeStack + (n == 2 ? 0u : 4u), "nested generated continuation restores the requested guest stack");
    }
    nestedExceptionTest = false;
    forcedResumeStack = 0;
    ctx.esp = 0xD000F000;
    X86_MEM32(ctx.esp) = 0xABCDEF01;
    X86_MEM32(ctx.esp + 4) = 0x10203040;
    X86_MEM32(ctx.esp + 8) = 0x2000;
    sub_00365CA1(ctx, base);
    check(ctx.eax == 0x12345678 && ctx.esp == 0xD000F00C && uint32_t(X86_MEM32(0xD000F008)) == 0xABCDEF01, "00365CA3 generated atomic stack exchange and indirect tail call");
    for (uint32_t n = 0; n < 3; ++n) {
      const uint32_t inputs[] = {0, 0x7FFFFFFF, 0xFFFFFFFF};
      ctx.esp = 0xD000F000;
      X86_MEM32(ctx.esp + 4) = 0x80000400;
      X86_MEM32(0x80000404) = inputs[n];
      sub_00452604(ctx, base);
      check(ctx.eax == inputs[n] + 1 && uint32_t(X86_MEM32(0xF0000404)) == inputs[n] + 1 && ctx.esp == 0xD000F008, "0045260E generated locked xadd wraps and returns incremented count");
    }
    memory.Map(0x5DB000, 0x1000);
    memory.Commit(0x5DB000, 0x1000);
    const uint8_t syntheticExtended[] = {0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0x3F};
    memory.Write(ctx, 0x5DB9B0, syntheticExtended, sizeof(syntheticExtended));
    ctx.top = 3;
    ctx.st[3] = 9;
    ctx.st[4] = 7;
    ctx.esp = 0xD000F000;
    sub_00452449(ctx, base);
    check(ctx.top == 3 && X87_ST(0) == 1.5 && ctx.st[4] == 7 && ctx.esp == 0xD000F004, "0045244B generated ten byte load uses scoped backing lease");
    constexpr uint32_t copyCapacity = (1u << 20) + 4096;
    memory.Map(0x01000000, copyCapacity);
    memory.Map(0x02000000, copyCapacity);
    memory.Commit(0x01000000, copyCapacity);
    memory.Commit(0x02000000, copyCapacity);
    std::vector<uint8_t> sourceBytes(copyCapacity), copyBytes(copyCapacity, 0xEE);
    for (uint32_t n = 0; n < copyCapacity; ++n) { sourceBytes[n] = uint8_t(n * 37 + 11); }
    memory.Write(ctx, 0x01000000, sourceBytes.data(), sourceBytes.size());
    const auto realCopy = [&](uint32_t length) {
      ctx.esp = 0xD000F000;
      X86_MEM32(ctx.esp + 4) = 0x02000001;
      X86_MEM32(ctx.esp + 8) = 0x01000001;
      X86_MEM32(ctx.esp + 12) = length;
      ctx.esi = 0x12345678; ctx.edi = 0x87654321; ctx.df = 0;
      sub_00309660(ctx, base);
      check(ctx.eax == length && ctx.ecx == 0 && ctx.esi == 0x12345678 && ctx.edi == 0x87654321 && ctx.esp == 0xD000F004, "00309660 real variable copy preserves registers and guest stack");
    };
    for (uint32_t length : {0u, 1u, 3u, 4u, 65539u, (1u << 20) + 3}) {
      std::fill(copyBytes.begin(), copyBytes.end(), 0xEE);
      memory.Write(ctx, 0x02000000, copyBytes.data(), copyBytes.size());
      realCopy(length);
      memory.Read(ctx, 0x02000000, copyBytes.data(), copyBytes.size());
      for (uint32_t n = 0; n < copyCapacity; ++n) {
        check(copyBytes[n] == (n > 0 && n <= length ? sourceBytes[n] : 0xEE), "00309673 real variable copy transfers requested bytes only");
      }
    }
    const uint32_t savedEsp = ctx.esp;
    try { runtime.Call(ctx, base, 0x2300, {}); check(false, "nested continuation unwinds"); }
    catch (const X86Resume& resume) {
      check(ctx.esp == savedEsp - 4 && resume.esp == savedEsp - 4, "nested continuation retains guest stack depth without normal return restoration");
    }
    fault([&] { x86_icall(ctx, base, 0xBAD); }, "unknown target diagnostic");
    fault([&] { x86_resume(ctx, base, X86Resume{0xBAD, 0}); }, "unknown resume diagnostic");
    ctx.ecx = 0x10000;
    X86_MEM32(0x1001C) = 0xFFFFFFFF;
    ctx.esp = 0xD000F000;
    __imp__sub_0016E504(ctx, base);
    check(ctx.eax == 0xFFFFFFE4 && ctx.esp == 0xD000F004, "0016E504 signed multiply wraps at 32 bits");
    ctx.esp = 0xD000F000;
    sub_0016E504(ctx, base);
    check(ctx.eax == 0xFFFFFFE5 && ctx.esp == 0xD000F004, "compact direct strong override calls original body");
    ctx.esp = 0xD000F000;
    x86_icall(ctx, base, 0x16E504);
    check(ctx.eax == 0xFFFFFFE5 && ctx.esp == 0xD000F004, "compact mapping selects strong override");
    ctx.esp = 0xD000F000; ctx.fs_base = 0x10800; ctx.eax = 5;
    X86_MEM32(ctx.fs_base) = 0x11223344;
    sub_002CDA10(ctx, base);
    check(ctx.esp == 0xD000EFF8 && ctx.eax == 0x1122334E, "real 002CDA10 catch resumes below generated frame");
    check(uint32_t(X86_MEM32(ctx.fs_base)) == 0xD000EFF4 && uint32_t(X86_MEM32(0xD000EFF4)) == 0x11223344, "real 002CDA10 installs guest exception record");
    for (uint32_t n = 0; n < 2; ++n) {
      ctx.esp = 0xD000F000;
      forcedResumeStack = ctx.esp + n * 4;
      bool propagated = false;
      try { sub_002CDA10(ctx, base); }
      catch (const X86Resume& resume) { propagated = resume.target == 0x2100 && resume.esp == forcedResumeStack; }
      check(propagated, "real 002CDA10 catch propagates at or above generated frame");
    }
    forcedResumeStack = 0;
    const X86FuncMapping duplicateMappings[] = {{0x2000, callback}, {0x2000, nested}};
    fault([&] { Runtime invalid(duplicateMappings, {}); }, "duplicate dispatch metadata rejected");
    const X86FuncMapping nullMapping[] = {{0x2000, nullptr}};
    fault([&] { Runtime invalid(nullMapping, {}); }, "null dispatch body rejected");
    const uint32_t missingContinuation[] = {0xBAD};
    fault([&] { Runtime invalid(mappings, missingContinuation); }, "continuation without body rejected");
    const uint32_t treeRoot = 0x10500;
    X86_MEM32(0x5E3E00) = treeRoot;
    X86_MEM32(treeRoot) = 0x10600;
    X86_MEM32(treeRoot + 4) = 0x10700;
    X86_MEM32(treeRoot + 0x10) = 20;
    X86_MEM32(0x10610) = 10;
    X86_MEM32(0x10710) = 30;
    for (uint32_t n = 0; n < 7; ++n) {
      const uint32_t inputs[] = {0x80000000, 9, 10, 20, 30, 31, 0x7FFFFFFF};
      ctx.esp = 0xD000F000; ctx.edx = inputs[n]; ctx.esi = 0xAABBCCDD;
      sub_00011013(ctx, base);
      check(ctx.eax == (n >= 2 && n <= 4 ? 1u : 0u) && ctx.esi == 0xAABBCCDD && ctx.esp == 0xD000F004, "00011024..00011046 signed tree branches");
    }
    ctx.fcw = 0;
    std::fesetround(FE_UPWARD);
    check(x87_to_i32(ctx, 2.5) == 2, "x87 guest rounding independent of host rounding");
    std::fesetround(FE_TONEAREST);
    ctx.mxcsr = 0x2000;
    check(sse_cvt(2.9f) == 2 && sse_cvt(-2.1f) == -3, "SSE conversion uses MXCSR round down");
    ctx.mxcsr = 0;
    check(x87_to_i32(ctx, 2.5) == 2 && x87_to_i32(ctx, 3.5) == 4, "x87 ties to even");
    ctx.fcw = 0x400;
    check(x87_to_i32(ctx, -2.1) == -3, "x87 round down");
    check(x87_to_i32(ctx, NAN) == INT32_MIN && sse_cvtt(INFINITY) == INT32_MIN, "floating integer indefinite");
    check(std::signbit(sse_min(0.0f, -0.0f)) && !std::signbit(sse_max(-0.0f, 0.0f)), "SSE equal selects second signed zero");
    check(std::isnan(sse_min(1, NAN)) && sse_max(NAN, 1) == 1, "SSE NaN selects second");
    {
      auto clock = runtime.BindClock();
      runtime.ticks = 733333333;
      check(x86_rdtsc() == 733333333, "controllable clock");
      {
        Runtime nestedClock(mappings, continuations);
        auto nestedBinding = nestedClock.BindClock();
        nestedClock.ticks = 41;
        check(x86_rdtsc() == 41, "nested thread clock binding");
      }
      check(x86_rdtsc() == 733333333, "thread clock binding restores prior runtime");
    }
    fault([&] { x86_rdtsc(); }, "thread clock binding cannot outlive runtime scope");
    {
      auto lease = memory.Borrow();
      fault([&] { runtime.Wait(ctx, runtime.Sequence(), std::chrono::milliseconds(0)); }, "guest wait cannot retain a host backing lease needed by its completion worker");
      fault([&] { runtime.Join(); }, "joining workers cannot retain a host backing lease");
    }
    const uint64_t sequence = runtime.Sequence();
    std::thread completer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      runtime.Wake();
    });
    check(runtime.Wait(ctx, sequence, std::chrono::seconds(1)) == Runtime::WaitResult::Changed, "independent completion worker wakes blocked guest");
    completer.join();
    check(runtime.Wait(ctx, sequence, std::chrono::milliseconds(0)) == Runtime::WaitResult::Changed, "completion before wait is not lost");
    check(runtime.Wait(ctx, runtime.Sequence(), std::chrono::milliseconds(5)) == Runtime::WaitResult::TimedOut, "guest wait has a bounded steady deadline");
    fault([&] { runtime.Wait(ctx, runtime.Sequence(), std::chrono::milliseconds(-1)); }, "negative wait budget rejected");
    Runtime cancelled(mappings, continuations);
    std::atomic<bool> waiting = false, cancellationObserved = false;
    std::thread waiter([&] {
      X86Context workerContext{};
      waiting.store(true);
      try { cancelled.Wait(workerContext, cancelled.Sequence(), std::chrono::seconds(10)); }
      catch (const browser::Cancelled&) { cancellationObserved.store(true); }
    });
    while (!waiting.load()) { std::this_thread::yield(); }
    cancelled.Cancel();
    waiter.join();
    check(cancellationObserved.load(), "cancellation wakes outstanding guest wait");
    ctx.host = &cancelled;
    bool stoppedDispatch = false;
    try { x86_icall(ctx, base, 0x2000); }
    catch (const browser::Cancelled&) { stoppedDispatch = true; }
    check(stoppedDispatch, "cancelled runtime cannot dispatch more guest work");
    bool stoppedBackedge = false;
    try { X86_BARRIER(); }
    catch (const browser::Cancelled&) { stoppedBackedge = true; }
    check(stoppedBackedge, "cancelled runtime stops generated loop backedges");
    ctx.esi = 0x10000; ctx.edi = 0x11000; ctx.ecx = 4096;
    bool stoppedCopy = false;
    try { x86_rep_movs(ctx, base, 1); }
    catch (const browser::Cancelled&) { stoppedCopy = true; }
    check(stoppedCopy && ctx.ecx == 4096, "cancelled string operation stops before modifying guest state");
    ctx.host = &runtime;
    double maximumStopMs = 0;
    for (uint32_t n = 0; n < 3; ++n) {
      Runtime capacityRuntime(mappings, continuations);
      workersEntered.store(0);
      capacityRuntime.ticks = n + 42;
      capacityRuntime.Launch(ctx, base, waitingWorker);
      capacityRuntime.Launch(ctx, base, waitingWorker);
      const auto entryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (workersEntered.load() < 2 && std::chrono::steady_clock::now() < entryDeadline) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
      check(workersEntered.load() == 2, "precreated workers start without yielding the guest to its own event loop");
      fault([&] { capacityRuntime.Launch(ctx, base, waitingWorker); }, "insufficient worker capacity gives a diagnostic before creating another worker");
      check(capacityRuntime.ActiveWorkers() == 2, "owned guest workers occupy bounded slots");
      const auto stopStarted = std::chrono::steady_clock::now();
      const auto stopFailure = capacityRuntime.Stop();
      const double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stopStarted).count();
      maximumStopMs = std::max(maximumStopMs, stopMs);
      check(!stopFailure && capacityRuntime.ActiveWorkers() == 0 && stopMs < 2000, "stop joins owned guest workers and cancels waits within a bounded deadline");
#ifdef __EMSCRIPTEN__
      if (n == 0) { std::printf("worker-stack-samples peak=%zu budget=%zu\n", capacityRuntime.StackPeak(), capacityRuntime.StackBudget()); }
#endif
      fault([&] { capacityRuntime.Launch(ctx, base, waitingWorker); }, "stopped generation cannot create workers");
    }
    std::printf("worker-lifecycle maximum_stop_ms=%.3f\n", maximumStopMs);
    Runtime reusableRuntime(mappings, continuations);
    for (uint32_t n = 0; n < 3; ++n) {
      ctx.thread_handle = n + 10;
      reusableRuntime.Launch(ctx, base, completingWorker);
      check(!reusableRuntime.Join() && uint32_t(X86_MEM32(0x80000600)) == n + 10, "completed worker slots can be reused with distinct guest contexts");
    }
    Runtime selfJoiningRuntime(mappings, continuations);
    selfJoiningRuntime.Launch(ctx, base, joiningWorker);
    const auto selfJoinFailure = selfJoiningRuntime.Join();
    check(bool(selfJoinFailure), "guest worker cannot deadlock by joining its own runtime");
    Runtime failingRuntime(mappings, continuations);
    failingRuntime.Launch(ctx, base, waitingWorker);
    failingRuntime.Launch(ctx, base, failingWorker);
    const auto workerFailure = failingRuntime.Join();
    check(bool(workerFailure) && failingRuntime.ActiveWorkers() == 0, "guest worker faults are retained after joining");
    fault([&] { std::rethrow_exception(workerFailure); }, "guest worker fault remains typed and observable");
#ifdef __EMSCRIPTEN__
    const size_t fixtureStack = runtime.StackPeak();
    const auto probeStack = [&](auto& self, unsigned depth) -> void {
      volatile uint8_t bytes[8192];
      bytes[0] = uint8_t(depth);
      __asm__ volatile("" : : "r"(bytes) : "memory");
      runtime.Checkpoint(ctx);
      if (depth) { self(self, depth - 1); }
      check(bytes[0] == depth, "nested stack probe retains distinct frames");
    };
    probeStack(probeStack, 3);
    check(runtime.StackPeak() >= fixtureStack + 16384 && runtime.StackPeak() < runtime.StackBudget(), "sampled nesting stays below the measured worker stack budget");
    std::printf("stack-samples fixture_peak=%zu nested_peak=%zu budget=%zu\n", fixtureStack, runtime.StackPeak(), runtime.StackBudget());
#endif
    const auto benchmark = [](const char* name, uint32_t iterations, auto operation) {
      const auto start = std::chrono::steady_clock::now();
      for (uint32_t n = 0; n < iterations; ++n) { operation(n); }
      const double elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
      std::printf("benchmark %s iterations=%u ns_per_op=%.2f\n", name, iterations, elapsed / iterations);
    };
    benchmark("translation", 100000, [&](uint32_t n) { X86_MEM32(0x20000 + (n & 0xFFFC)) = n; });
    benchmark("bulk_64k", 128, [&](uint32_t) { memory.Copy(ctx, 0x30000, 0x20000, 65536); });
    {
      auto lease = memory.Borrow();
      auto* contiguous = reinterpret_cast<uint32_t*>(memory.Span(ctx, 0x20000, 65536));
      benchmark("batched_span_alternative", 100000, [&](uint32_t n) { contiguous[(n & 0xFFFC) / 4] = n; });
    }
    for (uint32_t n = 0; n < 65536; n += 4) {
      check(uint32_t(X86_MEM32(0x20000 + n)) == uint32_t(X86_MEM32(0x30000 + n)), "batched span alternative agrees with validated scalar writes");
    }
    benchmark("dispatch", 100000, [&](uint32_t) { ctx.esp = 0xD000F000; x86_icall(ctx, base, 0x2000); });
    benchmark("real_sparse_stores", 100000, [&](uint32_t) { ctx.esp = 0xD000F000; ctx.ecx = 0x10001; sub_00011000(ctx, base); });
    benchmark("real_imul", 100000, [&](uint32_t) { ctx.esp = 0xD000F000; ctx.ecx = 0x10000; __imp__sub_0016E504(ctx, base); });
    benchmark("real_branches", 100000, [&](uint32_t n) { ctx.esp = 0xD000F000; ctx.edx = n & 63; sub_00011013(ctx, base); });
    benchmark("real_copy_1MiB", 3, [&](uint32_t) { realCopy(1u << 20); });
    check(uint32_t(X86_MEM32(0x30000)) == uint32_t(X86_MEM32(0x20000)), "optimized bulk result");
#ifdef __EMSCRIPTEN__
    std::printf("wasm-memory bytes=%zu maximum=%zu\n", emscripten_get_heap_size(), emscripten_get_heap_max());
#endif
    std::printf("cpu-runtime-ok backing=%zu\n", memory.BackingBytes());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "CPU fixture: %s\n", error.what());
    return 1;
  }
}
