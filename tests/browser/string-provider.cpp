#include "browser/string-provider.h"
#include <array>
#include <cstdio>
#include <string>

namespace {

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

struct Session {
  browser::Runtime runtime{{}, {}};
  browser::PreparedImage image;

  Session() {
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.memory->Ensure(0x20000, 4096);
    image.memory->Ensure(0x21000, 4096);
    image.memory->Ensure(0x40000, 0x11000);
    image.memory->Map(0x60000, 4096);
    image.memory->Ensure(0xfffff000, 4096);
    browser::InstallStringProvider(runtime, image);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 3> imports = {0x80000121, 0x80000117, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 289}, {0x10004, 279}};
    browser::BindImageImports(image, runtime, {});
  }

  void Frame(X86Context& context, uint32_t destination, uint32_t source) {
    context.esp = 0x20f00;
    context.last_icall = runtime.KernelTarget(289);
    const std::array<uint32_t, 3> frame = {0x12345678, destination, source};
    image.memory->Write(context, context.esp, frame.data(), sizeof(frame));
  }

  void Invoke(X86Context& context, uint8_t* base) {
    runtime.Invoke(context, base, runtime.KernelTarget(289));
  }

  void Reject(X86Context& context, uint8_t* base, uint32_t destination, uint32_t source) {
    Frame(context, destination, source);
    const auto previous = context;
    std::vector<uint8_t> before(0x2000), after(before.size());
    image.memory->Read(context, 0x20000, before.data(), before.size());
    bool refused = false;
    try { Invoke(context, base); }
    catch (const browser::Fault&) { refused = true; }
    image.memory->Read(context, 0x20000, after.data(), after.size());
    check(refused && before == after && std::memcmp(&previous, &context, sizeof(context)) == 0,
      "rejected string initialization mutated memory or CPU state");
  }

  void Initialize(X86Context& context, uint8_t* base, uint32_t destination, uint32_t source,
      uint16_t length, uint16_t maximum) {
    Frame(context, destination, source);
    const auto previous = context;
    std::vector<uint8_t> before(0x2000), after(before.size());
    image.memory->Read(context, 0x20000, before.data(), before.size());
    Invoke(context, base);
    auto expected = previous;
    expected.esp += 12;
    check(std::memcmp(&expected, &context, sizeof(context)) == 0, "void stdcall altered EAX, unrelated state or stack cleanup");
    std::array<uint32_t, 2> descriptor{};
    image.memory->Read(context, destination, descriptor.data(), sizeof(descriptor));
    check(descriptor[0] == (uint32_t(maximum) << 16 | length) && descriptor[1] == source,
      "ANSI_STRING8 lengths or original Buffer pointer incorrect");
    const size_t offset = destination - 0x20000;
    std::memcpy(before.data() + offset, descriptor.data(), sizeof(descriptor));
    image.memory->Read(context, 0x20000, after.data(), after.size());
    check(before == after, "initialization changed bytes outside ANSI_STRING8");
  }
};

void Contracts() {
  Session session;
  auto& runtime = session.runtime;
  auto& image = session.image;
  X86Context context{};
  context.host = &runtime;
  context.fs_base = 0x90000;
  context.eax = 0xfedc1234;
  context.ecx = 0x11223344;
  context.edx = 0xaabbccdd;
  context.ebx = 0x80000000;
  context.ebp = 0x20380;
  context.esi = 0xabcdef;
  context.edi = 0x123456;
  context.cf = 1;
  context.df = 1;
  context.fcw = 0x37f;
  context.xmm[0].q[0] = 0x123456789abcdef;
  session.Reject(context, image.memory->Base(), 0x20100, 0);
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    session.Initialize(ctx, base, 0x20100, 0, 0, 0);
    const std::array<uint8_t, 6> source = {'a', 0x80, 0xff, 'b', 0, 'x'};
    image.memory->Write(ctx, 0x20200, source.data(), source.size());
    session.Initialize(ctx, base, 0x20100, 0x20200, 4, 5);
    session.Initialize(ctx, base, 0x20100, 0x20204, 0, 1);
    session.Initialize(ctx, base, 0x20101, 0x20201, 3, 4);
    session.Initialize(ctx, base, 0x20f00, 0x20200, 4, 5);
    session.Initialize(ctx, base, 0x20f04, 0x20200, 4, 5);
    session.Initialize(ctx, base, 0x20f08, 0x20200, 4, 5);
    session.Initialize(ctx, base, 0x20100, 0x20f00, 4, 5);
    const std::array<uint8_t, 3> crossing = {'a', 'b', 0};
    image.memory->Write(ctx, 0x20fff, crossing.data(), crossing.size());
    session.Initialize(ctx, base, 0x20100, 0x20fff, 2, 3);
    session.Initialize(ctx, base, 0x20ffc, 0x20200, 4, 5);
    const uint8_t terminator = 0;
    image.memory->Write(ctx, 0x21fff, &terminator, 1);
    session.Initialize(ctx, base, 0x20100, 0x21fff, 0, 1);
    image.memory->Write(ctx, 0xffffffff, &terminator, 1);
    session.Initialize(ctx, base, 0x20100, 0xffffffff, 0, 1);
    std::string longest(65534, 'q');
    longest.push_back(0);
    image.memory->Write(ctx, 0x40000, longest.data(), longest.size());
    session.Initialize(ctx, base, 0x20100, 0x40000, 65534, 65535);
    std::string retained(longest.size(), 0);
    image.memory->Read(ctx, 0x40000, retained.data(), retained.size());
    check(retained == longest, "source string copied or modified");
    longest[65534] = 'q';
    longest.push_back(0);
    image.memory->Write(ctx, 0x40000, longest.data(), longest.size());
    session.Reject(ctx, base, 0x20100, 0x40000);
    longest.back() = 'q';
    image.memory->Write(ctx, 0x40000, longest.data(), longest.size());
    session.Reject(ctx, base, 0x20100, 0x40000);
    const uint8_t nonzero = 'q';
    image.memory->Write(ctx, 0x21fff, &nonzero, 1);
    image.memory->Write(ctx, 0xffffffff, &nonzero, 1);
    for (const uint32_t sourceAddress : {0x21fffu, 0x22000u, 0x60000u, 0xffffffffu}) {
      session.Reject(ctx, base, 0x20100, sourceAddress);
    }
    for (const uint32_t destination : {0u, 0x21ffcu, 0x22000u, 0x60000u, 0xfffffffcu,
        0x20200u, 0x201fcu, 0x20204u}) {
      session.Reject(ctx, base, destination, 0x20200);
    }
    for (const uint32_t stack : {0x21ff8u, 0x22000u, 0x60000u, 0xfffffff8u}) {
      ctx.esp = stack;
      const auto previous = ctx;
      bool refused = false;
      try { session.Invoke(ctx, base); }
      catch (const browser::Fault&) { refused = true; }
      check(refused && std::memcmp(&previous, &ctx, sizeof(ctx)) == 0, "invalid stack frame mutated CPU state");
    }
    browser::Runtime foreign({}, {});
    auto alien = ctx;
    alien.host = &foreign;
    session.Reject(alien, base, 0x20100, 0);
    alien = ctx;
    alien.fs_base += 0x1000;
    session.Reject(alien, base, 0x20100, 0);
    browser::Memory foreignMemory;
    session.Reject(ctx, nullptr, 0x20100, 0);
    session.Reject(ctx, foreignMemory.Base(), 0x20100, 0);
    {
      auto lease = image.memory->Borrow();
      session.Reject(ctx, base, 0x20100, 0);
    }
    {
      auto lease = foreignMemory.Borrow();
      session.Reject(ctx, base, 0x20100, 0);
    }
    image.importsBound = false;
    session.Reject(ctx, base, 0x20100, 0);
    image.importsBound = true;
    image.boundRuntime = &foreign;
    session.Reject(ctx, base, 0x20100, 0);
    image.boundRuntime = &runtime;
    runtime.Cancel();
    session.Frame(ctx, 0x20100, 0);
    const auto previous = ctx;
    std::array<uint32_t, 2> before{}, after{};
    image.memory->Read(ctx, 0x20100, before.data(), sizeof(before));
    bool cancelled = false;
    try { session.Invoke(ctx, base); }
    catch (const browser::Cancelled&) { cancelled = true; }
    image.memory->Read(ctx, 0x20100, after.data(), sizeof(after));
    check(cancelled && before == after && std::memcmp(&previous, &ctx, sizeof(ctx)) == 0,
      "cancelled initialization mutated destination or CPU state");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(!runtime.ActiveWorkers(), "string session retained active workers");
}

struct EqualSession : Session {
  void Descriptor(uint32_t address, uint16_t length, uint32_t buffer, uint16_t maximum = UINT16_MAX) {
    const std::array<uint32_t, 2> descriptor = {length | (uint32_t(maximum) << 16), buffer};
    image.memory->Write({}, address, descriptor.data(), sizeof(descriptor));
  }

  void Frame(X86Context& context, uint32_t first = 0x20100, uint32_t second = 0x20110,
      uint32_t ignoreCase = 0, uint32_t stack = 0x20f00) {
    context.esp = stack;
    context.last_icall = runtime.KernelTarget(279);
    const std::array<uint32_t, 4> frame = {0x12345678, first, second, ignoreCase};
    image.memory->Write(context, stack, frame.data(), sizeof(frame));
  }

  void Invoke(X86Context& context, uint8_t* base) {
    runtime.Invoke(context, base, runtime.KernelTarget(279));
  }

  void Expect(X86Context& context, uint8_t* base, bool equal) {
    const auto previous = context;
    std::vector<uint8_t> before(0x2000), after(before.size());
    image.memory->Read(context, 0x20000, before.data(), before.size());
    Invoke(context, base);
    auto expected = previous;
    expected.eax = equal;
    expected.esp += 16;
    image.memory->Read(context, 0x20000, after.data(), after.size());
    check(before == after && std::memcmp(&expected, &context, sizeof(context)) == 0,
      "RtlEqualString changed memory or violated BOOLEAN stdcall state");
  }

  void Reject(X86Context& context, uint8_t* base, bool cancellation = false) {
    const auto previous = context;
    std::vector<uint8_t> before(0x2000), after(before.size());
    image.memory->Read(context, 0x20000, before.data(), before.size());
    bool refused = false;
    try { Invoke(context, base); }
    catch (const browser::Cancelled&) { refused = cancellation; }
    catch (const browser::Fault&) { refused = !cancellation; }
    image.memory->Read(context, 0x20000, after.data(), after.size());
    check(refused && before == after && std::memcmp(&previous, &context, sizeof(context)) == 0,
      "rejected RtlEqualString changed memory or CPU state");
  }
};

void EqualContracts() {
  EqualSession session;
  auto& runtime = session.runtime;
  auto& image = session.image;
  check(runtime.KernelTarget(279) == 0xfe00045c, "actual ordinal 279 RtlEqualString is unavailable");
  check(!runtime.KernelTarget(270) && !runtime.KernelTarget(316), "string provider enabled unrelated exports");
  X86Context context{};
  context.host = &runtime;
  context.fs_base = 0x90000;
  context.eax = 0xfedc1234;
  context.ecx = 0x11223344;
  context.edx = 0xaabbccdd;
  context.ebx = 0x80000000;
  context.ebp = 0x20380;
  context.esi = 0xabcdef;
  context.edi = 0x123456;
  context.cf = 1;
  context.df = 1;
  context.fcw = 0x37f;
  context.xmm[0].q[0] = 0x123456789abcdef;
  session.Frame(context);
  session.Reject(context, image.memory->Base());
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto compare = [&](uint16_t length, uint32_t mode, bool equal) {
      session.Descriptor(0x20100, length, 0x20200);
      session.Descriptor(0x20110, length, 0x20300);
      session.Frame(ctx, 0x20100, 0x20110, mode);
      session.Expect(ctx, base, equal);
    };
    const std::array<uint8_t, 6> first = {'A', 0, 'z', '[', 0x80, 0xff};
    auto second = first;
    image.memory->Write(ctx, 0x20200, first.data(), first.size());
    image.memory->Write(ctx, 0x20300, second.data(), second.size());
    compare(6, 0xffffff00, true);
    second[2] = 'Z';
    image.memory->Write(ctx, 0x20300, second.data(), second.size());
    compare(4, 0, false);
    for (const uint32_t mode : {1u, 2u, 0xffffff80u}) { compare(4, mode, true); }
    second[3] = '{';
    image.memory->Write(ctx, 0x20300, second.data(), second.size());
    compare(4, 1, false);
    compare(6, 0, false);
    for (const uint32_t mode : {1u, 0xffu}) {
      session.Frame(ctx, 0x20100, 0x20110, mode);
      session.Reject(ctx, base);
    }
    session.Descriptor(0x20100, 1, 0x22000);
    session.Descriptor(0x20110, 2, 0);
    session.Frame(ctx, 0x20100, 0x20110, 1);
    session.Expect(ctx, base, false);
    for (const uint32_t buffer : {0u, 0x22000u, 0x60000u, 0xffffffffu}) {
      session.Descriptor(0x20100, 0, buffer, 0);
      session.Descriptor(0x20110, 0, 0, 0);
      session.Frame(ctx, 0x20100, 0x20110, 1);
      session.Expect(ctx, base, true);
    }
    std::array<uint8_t, 256> bytes{};
    for (uint32_t n = 0; n < bytes.size(); ++n) { bytes[n] = uint8_t(n); }
    image.memory->Write(ctx, 0x40000, bytes.data(), bytes.size());
    session.Descriptor(0x20100, bytes.size(), 0x40000);
    session.Descriptor(0x20110, bytes.size(), 0x40000);
    session.Frame(ctx, 0x20100, 0x20110, 0x100);
    session.Expect(ctx, base, true);
    session.Frame(ctx, 0x20100, 0x20100, 1);
    session.Reject(ctx, base);
    const uint8_t high = 0xff;
    image.memory->Write(ctx, 0xffffffff, &high, 1);
    session.Descriptor(0x20100, 1, 0xffffffff);
    session.Descriptor(0x20110, 1, 0xffffffff);
    session.Frame(ctx);
    session.Expect(ctx, base, true);
    const std::array<uint8_t, 3> crossing = {'A', 0, 'b'};
    image.memory->Write(ctx, 0x20fff, crossing.data(), crossing.size());
    session.Descriptor(0x20101, 3, 0x20fff, 3);
    session.Frame(ctx, 0x20101, 0x20101, 1, 0x20f01);
    session.Expect(ctx, base, true);
    session.Frame(ctx, 0x20101, 0x20101, 1, 0xffffffef);
    session.Expect(ctx, base, true);
    session.Descriptor(0x20ffc, 3, 0x20fff, 3);
    session.Frame(ctx, 0x20ffc, 0x20ffc);
    session.Expect(ctx, base, true);
    session.Descriptor(0x20100, 8, 0x20100);
    session.Descriptor(0x20110, 8, 0x20100);
    session.Frame(ctx);
    session.Expect(ctx, base, true);
    session.Frame(ctx, 0x20f00, 0x20f00);
    const uint32_t emptyReturn = 0x12340000;
    image.memory->Write(ctx, ctx.esp, &emptyReturn, sizeof(emptyReturn));
    session.Expect(ctx, base, true);
    std::vector<uint8_t> longest(UINT16_MAX, 'q');
    image.memory->Write(ctx, 0x40000, longest.data(), longest.size());
    session.Descriptor(0x20100, UINT16_MAX, 0x40000);
    session.Descriptor(0x20110, UINT16_MAX, 0x40000);
    session.Frame(ctx, 0x20100, 0x20110, 1);
    session.Expect(ctx, base, true);
    image.memory->Write(ctx, 0x4fffe, &high, 1);
    session.Frame(ctx, 0x20100, 0x20110, 1);
    session.Reject(ctx, base);
    session.Frame(ctx);
    session.Expect(ctx, base, true);
    for (const uint32_t buffer : {0u, 0x21fffu, 0x22000u, 0x60000u, 0xffffffffu}) {
      session.Descriptor(0x20100, 2, buffer);
      session.Descriptor(0x20110, 2, 0x20200);
      session.Frame(ctx);
      session.Reject(ctx, base);
      session.Frame(ctx, 0x20110, 0x20100);
      session.Reject(ctx, base);
    }
    for (const uint32_t descriptor : {0u, 0x21ffcu, 0x22000u, 0x60000u, 0xfffffffcu}) {
      session.Frame(ctx, descriptor, 0x20110);
      session.Reject(ctx, base);
      session.Frame(ctx, 0x20110, descriptor);
      session.Reject(ctx, base);
    }
    for (const uint32_t stack : {0x21ff8u, 0x22000u, 0x60000u, 0xfffffff0u, 0xfffffff8u}) {
      ctx.esp = stack;
      ctx.last_icall = runtime.KernelTarget(279);
      session.Reject(ctx, base);
    }
    session.Descriptor(0x20100, 0, 0);
    session.Descriptor(0x20110, 0, 0);
    session.Frame(ctx);
    browser::Runtime foreign({}, {});
    auto alien = ctx;
    alien.host = &foreign;
    session.Reject(alien, base);
    alien = ctx;
    alien.fs_base += 4096;
    session.Reject(alien, base);
    browser::Memory foreignMemory;
    session.Reject(ctx, nullptr);
    session.Reject(ctx, foreignMemory.Base());
    {
      auto lease = image.memory->Borrow();
      session.Reject(ctx, base);
    }
    {
      auto lease = foreignMemory.Borrow();
      session.Reject(ctx, base);
    }
    image.importsBound = false;
    session.Reject(ctx, base);
    image.importsBound = true;
    image.boundRuntime = &foreign;
    session.Reject(ctx, base);
    image.boundRuntime = &runtime;
    runtime.Cancel();
    session.Reject(ctx, base, true);
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(!runtime.ActiveWorkers(), "equality session retained workers");
}

void CoherentEquality() {
  EqualSession session;
  auto& runtime = session.runtime;
  auto& memory = *session.image.memory;
  std::atomic<uint32_t> ready = 0;
  for (uint32_t n = 0; n < 2; ++n) {
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x90000 + n * 4096;
    const bool writer = n == 0;
    runtime.Launch(context, memory.Base(), [&, writer](X86Context& ctx, uint8_t* base) {
      ++ready;
      while (ready.load() != 2) { std::this_thread::yield(); }
      for (uint32_t n = 0; n < 1000; ++n) {
        if (writer) {
          auto transition = memory.Mutate();
          const uint8_t value = uint8_t(n);
          memory.Write(ctx, 0x20200, &value, 1);
          memory.Write(ctx, 0x20300, &value, 1);
          session.Descriptor(0x20100, 1, n % 2 ? 0x20200 : 0x20300);
          session.Descriptor(0x20110, 1, n % 2 ? 0x20300 : 0x20200);
        } else {
          session.Frame(ctx);
          const auto previous = ctx;
          session.Invoke(ctx, base);
          auto expected = previous;
          expected.eax = 1;
          expected.esp += 16;
          check(std::memcmp(&expected, &ctx, sizeof(ctx)) == 0,
            "equality observed incoherent descriptor or source snapshots");
        }
      }
    });
  }
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(!runtime.ActiveWorkers(), "coherent equality retained workers");
}

void ConcurrentWorkers() {
  Session first;
  Session second;
  std::atomic<uint32_t> ready = 0;
  for (Session* session : {&first, &second}) {
    for (uint32_t n = 0; n < 2; ++n) {
      X86Context context{};
      context.host = &session->runtime;
      context.fs_base = 0x90000 + n * 4096;
      context.esp = 0x20800 + n * 0x100;
      session->runtime.Launch(context, session->image.memory->Base(), [&, session, n](X86Context& ctx, uint8_t* base) {
        ++ready;
        while (ready.load() != 4) { std::this_thread::yield(); }
        const uint32_t stack = ctx.esp;
        const uint32_t destination = 0x20100 + n * 8;
        const uint32_t source = 0x20200 + n * 8;
        const std::array<uint8_t, 2> text = {uint8_t('A' + n), 0};
        session->image.memory->Write(ctx, source, text.data(), text.size());
        for (uint32_t n = 0; n < 100; ++n) {
          const std::array<uint32_t, 3> frame = {0x12345, destination, source};
          ctx.esp = stack;
          ctx.eax = n;
          session->image.memory->Write(ctx, stack, frame.data(), sizeof(frame));
          session->Invoke(ctx, base);
          std::array<uint32_t, 2> descriptor{};
          session->image.memory->Read(ctx, destination, descriptor.data(), sizeof(descriptor));
          check(descriptor[0] == 0x20001 && descriptor[1] == source && ctx.eax == n && ctx.esp == stack + 12,
            "concurrent string workers crossed sessions or lost ABI state");
          const std::array<uint32_t, 4> equalityFrame = {0x12345, destination, destination, 1};
          ctx.esp = stack;
          ctx.last_icall = session->runtime.KernelTarget(279);
          session->image.memory->Write(ctx, stack, equalityFrame.data(), sizeof(equalityFrame));
          session->runtime.Invoke(ctx, base, ctx.last_icall);
          check(ctx.eax == 1 && ctx.esp == stack + 16, "concurrent equality crossed sessions or lost ABI state");
        }
      });
    }
  }
  for (Session* session : {&first, &second}) {
    if (const auto failure = session->runtime.Join()) { std::rethrow_exception(failure); }
    check(!session->runtime.ActiveWorkers(), "concurrent string worker leaked");
  }
}

}

int main() {
  try {
    for (uint32_t n = 0; n < 3; ++n) { EqualContracts(); Contracts(); }
    CoherentEquality();
    ConcurrentWorkers();
    std::puts("string-provider-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
