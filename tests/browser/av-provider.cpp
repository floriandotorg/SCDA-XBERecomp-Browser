#include "browser/av-provider.h"
#include <array>
#include <cstdio>
#include <string_view>

using browser::Memory;
using browser::Runtime;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto refuse = [](const auto& operation, std::string_view reason = {}) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault& fault) {
    rejected = true;
    check(std::string_view(fault.what()).starts_with(reason), "fault omitted the saved-display blocker");
  }
  check(rejected, "invalid AV operation succeeded");
};
const auto put = [](Memory& memory, uint32_t address, uint32_t value) {
  memory.Write({}, address, &value, sizeof(value));
};
const auto sameRegisters = [](const X86Context& actual, const X86Context& expected) {
  check(actual.eax == expected.eax && actual.ecx == expected.ecx && actual.edx == expected.edx &&
    actual.ebx == expected.ebx && actual.esp == expected.esp && actual.ebp == expected.ebp &&
    actual.esi == expected.esi && actual.edi == expected.edi && actual.cf == expected.cf &&
    actual.zf == expected.zf && actual.fs_base == expected.fs_base && actual.host == expected.host,
    "AV operation changed unrelated registers or published a failed result");
};

struct Fixture {
  Runtime runtime{{}, {}};
  Memory memory;
  X86Context context{};
  Fixture() {
    memory.Ensure(0x10000, 0x2000);
    context.host = &runtime;
    context.fs_base = 0x10000;
    context.esp = 0x11800;
    context.eax = 0xabcdef01;
    context.ecx = 0x12345678;
    context.edx = 0x87654321;
    context.ebx = 0x11223344;
    context.ebp = 0x55667788;
    context.esi = 0x99aabbcc;
    context.edi = 0xddeeff00;
    context.cf = 1;
    context.zf = 1;
    put(memory, context.esp, 0x10203040);
    put(memory, context.esp + 4, 0);
  }
  void Install() { browser::InstallAvProvider(runtime, memory); }
  void Run(const auto& entry) {
    runtime.Launch(context, memory.Base(), entry);
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
};

const auto registrationContracts = [] {
  for (const uint32_t ordinal : {1u, 4u}) {
    Fixture fixture;
    fixture.runtime.RegisterKernel(ordinal, [](X86Context&, uint8_t*) {});
    const auto revision = fixture.runtime.KernelRevision();
    const auto backing = fixture.memory.BackingBytes();
    refuse([&] { fixture.Install(); });
    check(fixture.runtime.KernelRevision() == revision && fixture.memory.BackingBytes() == backing &&
      !fixture.runtime.KernelTarget(ordinal == 1 ? 4 : 1), "collision partially installed AV provider");
  }
  for (uint32_t n = 0; n < 4; ++n) {
    Fixture fixture;
    if (n == 0) { fixture.runtime.Cancel(); }
    if (n == 1) {
      fixture.runtime.PublishImageMemory(fixture.memory, fixture.runtime.KernelRevision(), [] {});
    }
    const auto revision = fixture.runtime.KernelRevision();
    const auto reject = [&] {
      refuse([&] { fixture.Install(); });
      check(!fixture.runtime.KernelTarget(1) && !fixture.runtime.KernelTarget(4) &&
        fixture.runtime.KernelRevision() == revision, "invalid installation changed AV registration");
    };
    if (n == 2) {
      auto lease = fixture.memory.Borrow();
      reject();
    } else if (n == 3) {
      fixture.Run([&](X86Context&, uint8_t*) { reject(); });
    } else {
      reject();
    }
  }
};

const auto savedDisplayContracts = [] {
  {
    Fixture fixture;
    browser::InstallAvProvider(fixture.runtime, fixture.memory, [] { return 0x80100000u; });
    fixture.Run([&](X86Context& worker, uint8_t* base) {
      auto request = worker;
      fixture.runtime.Kernel(request, base, 1);
      check(request.eax == 0x80100000, "restored display was not visible to the fresh runtime");
      request = worker;
      fixture.runtime.Kernel(request, base, 4);
      request = worker;
      fixture.runtime.Kernel(request, base, 1);
      check(request.eax == 0, "cleared restored display was resurrected");
    });
  }
  for (uint32_t n = 0; n < 3; ++n) {
    Fixture fixture;
    const auto backing = fixture.memory.BackingBytes();
    fixture.Install();
    check(fixture.runtime.KernelTarget(1) == 0xfe000004 && fixture.runtime.KernelTarget(4) == 0xfe000010,
      "saved display ordinals did not register as functions");
    check(!fixture.runtime.KernelTarget(2) && !fixture.runtime.KernelTarget(3),
      "AV provider invented encoder or display hardware effects");
    check(fixture.memory.BackingBytes() == backing, "cold boot invented persisted framebuffer storage");
    const auto revision = fixture.runtime.KernelRevision();
    refuse([&] { fixture.Install(); });
    check(fixture.runtime.KernelRevision() == revision, "duplicate installation replaced AV state");
    refuse([&] { fixture.runtime.Kernel(fixture.context, fixture.memory.Base(), 1); });
    refuse([&] { fixture.runtime.Kernel(fixture.context, fixture.memory.Base(), 4); });
    fixture.runtime.PublishImageMemory(fixture.memory, revision, [] {});
    fixture.Run([&](X86Context& worker, uint8_t* base) {
      const auto invoke = [&](uint32_t ordinal, uint32_t value = 0) {
        auto request = worker;
        put(fixture.memory, request.esp + 4, value);
        std::array<uint8_t, 0x2000> before{}, after{};
        fixture.memory.Read({}, 0x10000, before.data(), before.size());
        fixture.runtime.Kernel(request, base, ordinal);
        auto expected = worker;
        expected.eax = 0;
        expected.esp += ordinal == 1 ? 4 : 8;
        sameRegisters(request, expected);
        fixture.memory.Read({}, 0x10000, after.data(), after.size());
        check(before == after, "AV saved-address call changed guest data");
      };
      for (uint32_t n = 0; n < 20; ++n) {
        invoke(1);
        invoke(4);
      }
      for (const uint32_t address : {0x10000u, 0x10001u, 0x11fffu, 0x12000u, 0x80000000u, UINT32_MAX}) {
        auto request = worker;
        put(fixture.memory, request.esp + 4, address);
        refuse([&] { fixture.runtime.Kernel(request, base, 4); },
          "AV saved display requires lifetime-bound guest storage");
        sameRegisters(request, worker);
        invoke(1);
      }
      for (const uint32_t ordinal : {1u, 4u}) {
        for (const uint32_t address : {0x11ffeu, 0x12000u, 0xfffffffcu}) {
          auto request = worker;
          request.esp = address;
          const auto expected = request;
          refuse([&] { fixture.runtime.Kernel(request, base, ordinal); });
          sameRegisters(request, expected);
        }
        auto wrongContext = worker;
        ++wrongContext.fs_base;
        refuse([&] { fixture.runtime.Kernel(wrongContext, base, ordinal); });
        auto missingHost = worker;
        missingHost.host = nullptr;
        refuse([&] { fixture.runtime.Kernel(missingHost, base, ordinal); });
        refuse([&] { fixture.runtime.Kernel(worker, nullptr, ordinal); });
        Memory foreign;
        refuse([&] { fixture.runtime.Kernel(worker, foreign.Base(), ordinal); });
        {
          auto lease = fixture.memory.Borrow();
          refuse([&] { fixture.runtime.Kernel(worker, base, ordinal); });
        }
        invoke(1);
      }
      check(fixture.memory.BackingBytes() == backing, "AV calls allocated a persisted framebuffer");
      fixture.runtime.Cancel();
      for (const uint32_t ordinal : {1u, 4u}) {
        auto request = worker;
        refuse([&] { fixture.runtime.Kernel(request, base, ordinal); });
        sameRegisters(request, worker);
      }
    });
  }
};

const auto isolationContracts = [] {
  Fixture first;
  first.Install();
#ifndef __EMSCRIPTEN__
  Fixture second;
  second.Install();
  first.Run([&](X86Context& worker, uint8_t* base) {
    const auto before = worker;
    refuse([&] { second.runtime.Kernel(worker, base, 1); });
    sameRegisters(worker, before);
    check(first.runtime.Call(worker, base, first.runtime.KernelTarget(1), {}) == 0,
      "fresh session inherited a saved display");
    second.Run([&](X86Context& other, uint8_t* otherBase) {
      check(second.runtime.Call(other, otherBase, second.runtime.KernelTarget(4), {0}) == 0,
        "isolated null clear failed");
      check(second.runtime.Call(other, otherBase, second.runtime.KernelTarget(1), {}) == 0,
        "isolated session inherited a saved display");
    });
    check(first.runtime.Call(worker, base, first.runtime.KernelTarget(1), {}) == 0,
      "another session changed this saved display");
  });
#else
  first.Run([&](X86Context& worker, uint8_t* base) {
    check(first.runtime.Call(worker, base, first.runtime.KernelTarget(1), {}) == 0,
      "fresh Wasm session inherited a saved display");
    check(first.runtime.Call(worker, base, first.runtime.KernelTarget(4), {0}) == 0,
      "Wasm runtime thunk null clear failed");
  });
#endif
};

int main() {
  registrationContracts();
  savedDisplayContracts();
  isolationContracts();
  std::puts("av-provider-contracts-ok");
}
