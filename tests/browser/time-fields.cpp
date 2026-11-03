#include "browser/timer-provider.h"
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include "native-time-fields.h"

using Fields = std::array<int16_t, 8>;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

const auto reference = [](int64_t ticks) {
  using namespace std::chrono;
  const int64_t millisecondsSince1601 = ticks / 10000 - int64_t(ticks % 10000 < 0);
  const sys_time<milliseconds> time{milliseconds{millisecondsSince1601 - 11644473600000ll}};
  const auto date = floor<days>(time);
  const year_month_day civil{date};
  const hh_mm_ss clock{time - date};
  return Fields{int16_t(int(civil.year())), int16_t(unsigned(civil.month())), int16_t(unsigned(civil.day())),
    int16_t(clock.hours().count()), int16_t(clock.minutes().count()), int16_t(clock.seconds().count()),
    int16_t(clock.subseconds().count()), int16_t(weekday{date}.c_encoding())};
};

const auto ticksFor = [](int year, unsigned month, unsigned day) {
  using namespace std::chrono;
  return (sys_days{std::chrono::year{year} / month / day}.time_since_epoch().count() + 134774ll) * 864000000000ll;
};

int main() {
  browser::Runtime runtime({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 4096);
  image.memory->Ensure(0x20000, 4096);
  image.memory->Ensure(0x21000, 4096);
  image.memory->Ensure(0x40000, 4096);
  image.memory->Map(0x30000, 4096);
  browser::InstallTimerProvider(runtime, image);
  check(runtime.KernelTarget(305) != 0, "ordinal 305 is missing from existing timer provider");
  check(runtime.KernelTarget(304) == 0, "conversion installed speculative ordinal 304");
  X86Context context{};
  context.host = &runtime;
  const auto prepare = [&](X86Context& ctx, uint32_t input, uint32_t output, uint32_t stack = 0x21f00) {
    const std::array<uint32_t, 3> frame = {0x12345, input, output};
    image.memory->Write({}, stack, frame.data(), sizeof(frame));
    ctx.esp = stack;
    ctx.eax = 0xfeedabcd;
    ctx.ecx = 0x12345678;
    ctx.edx = 0x87654321;
    ctx.ebx = 0x654321;
    ctx.ebp = 0x765432;
    ctx.esi = 0x876543;
    ctx.edi = 0x987654;
    ctx.xflags = 0x246;
  };
  const auto reject = [&](X86Context& ctx, uint8_t* base) {
    std::array<uint8_t, 8192> before{}, after{};
    image.memory->Read({}, 0x20000, before.data(), before.size());
    const auto saved = ctx;
    bool refused = false;
    try { runtime.Kernel(ctx, base, 305); }
    catch (const browser::Fault&) { refused = true; }
    image.memory->Read({}, 0x20000, after.data(), after.size());
    check(refused && ctx.esp == saved.esp && ctx.eax == saved.eax && ctx.ecx == saved.ecx &&
      ctx.edx == saved.edx && ctx.xflags == saved.xflags && before == after,
      "rejected conversion changed output, registers or stack");
  };
  prepare(context, 0x20100, 0x20200);
  reject(context, image.memory->Base());
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) { reject(ctx, base); });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  image.image.kernel_thunk = 0x10000;
  const std::array<uint32_t, 2> imports = {0x80000131, 0};
  image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
  image.imports = {{0x10000, 305}};
  browser::BindImageImports(image, runtime, {});
  check(image.boundImports.size() == 1 && image.boundImports[0].kind == browser::ImportKind::Function,
    "time fields did not bind as production function");
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto convert = [&](int64_t ticks, uint32_t input = 0x20100, uint32_t output = 0x20200) {
      std::array<uint8_t, 24> sentinel;
      sentinel.fill(0xa5);
      image.memory->Write({}, output - 4, sentinel.data(), sentinel.size());
      prepare(ctx, input, output);
      image.memory->Write({}, input, &ticks, sizeof(ticks));
      std::array<uint8_t, 4> prefix{}, suffix{}, prefixAfter{}, suffixAfter{};
      image.memory->Read({}, output - 4, prefix.data(), prefix.size());
      image.memory->Read({}, output + 16, suffix.data(), suffix.size());
      runtime.Kernel(ctx, base, 305);
      check(ctx.esp == 0x21f0c && ctx.eax == 0xfeedabcd && ctx.ecx == 0x12345678 && ctx.edx == 0x87654321 &&
        ctx.ebx == 0x654321 && ctx.ebp == 0x765432 && ctx.esi == 0x876543 && ctx.edi == 0x987654 && ctx.xflags == 0x246,
        "void stdcall ABI changed registers or stack cleanup");
      Fields actual;
      image.memory->Read({}, output, actual.data(), sizeof(actual));
      image.memory->Read({}, output - 4, prefixAfter.data(), prefixAfter.size());
      image.memory->Read({}, output + 16, suffixAfter.data(), suffixAfter.size());
      check(prefix == prefixAfter && suffix == suffixAfter, "conversion wrote outside TIME_FIELDS 16-byte output");
      check(actual == reference(ticks), "UTC fields differ from independent chrono reference");
      if (ticks >= 0 || ticks % 10000000 == 0) {
        Fields native;
        NativeTimeToTimeFields(std::bit_cast<uint64_t>(ticks), native);
        check(actual == native, "valid native NT time differs from actual native ordinal 305 body");
      }
      return actual;
    };
    check(convert(0) == Fields{1601, 1, 1, 0, 0, 0, 0, 1}, "NT epoch is not Monday");
    check(convert(116444736000000000ll) == Fields{1970, 1, 1, 0, 0, 0, 0, 4}, "Unix epoch is not Thursday");
    check(convert(ticksFor(2000, 2, 29)) == Fields{2000, 2, 29, 0, 0, 0, 0, 2}, "2000 leap century mismatch");
    check(convert(ticksFor(1900, 3, 1)) == Fields{1900, 3, 1, 0, 0, 0, 0, 4}, "1900 nonleap century mismatch");
    check(convert(ticksFor(2100, 3, 1)) == Fields{2100, 3, 1, 0, 0, 0, 0, 1}, "2100 nonleap century mismatch");
    check(convert(ticksFor(2024, 2, 29) + 452967891234ll) == Fields{2024, 2, 29, 12, 34, 56, 789, 4},
      "fixed UTC input or 100ns fraction truncation mismatch");
    check(convert(-1) == Fields{1600, 12, 31, 23, 59, 59, 999, 0}, "negative fraction must floor into previous date");
    check(convert(-10000000) == Fields{1600, 12, 31, 23, 59, 59, 0, 0}, "negative whole second mismatch");
    const int64_t publicUtcSample = int64_t(browser::Runtime::SystemTime());
    const auto publicUtcFields = convert(publicUtcSample);
    std::printf("public-utc-input-100ns=%lld fields=%d/%d/%d %d:%d:%d.%03d weekday=%d\n",
      static_cast<long long>(publicUtcSample), publicUtcFields[0], publicUtcFields[1], publicUtcFields[2],
      publicUtcFields[3], publicUtcFields[4], publicUtcFields[5], publicUtcFields[6], publicUtcFields[7]);
    for (const int64_t ticks : {INT64_MIN, INT64_MIN + 1, INT64_MAX - 1, INT64_MAX, int64_t(-10001), int64_t(-10000),
        int64_t(-9999), int64_t(1), int64_t(9999), int64_t(10000), int64_t(9999999), int64_t(10000000)}) {
      convert(ticks);
    }
    for (const int year : {0, 1, 4, 100, 400, 1599, 1600, 1601, 1899, 1900, 1999, 2000, 2099, 2100, 2400, 30800}) {
      for (unsigned n = 1; n <= 12; ++n) {
        const auto boundary = ticksFor(year, n, 1);
        for (const int64_t delta : {-864000000001ll, -10000001ll, -10001ll, -1ll, 0ll, 1ll, 10001ll, 10000001ll, 864000000001ll}) {
          convert(boundary + delta);
        }
      }
    }
    uint64_t bits = 0x7f3914dce123a987;
    for (uint32_t n = 0; n < 10000; ++n) {
      bits ^= bits << 13;
      bits ^= bits >> 7;
      bits ^= bits << 17;
      convert(std::bit_cast<int64_t>(bits));
    }
    for (const auto [input, output] : std::array<std::array<uint32_t, 2>, 7>{{
        {0x20101, 0x20201}, {0x20100, 0x20100}, {0x20104, 0x20100}, {0x20100, 0x20104},
        {0x20100, 0x20ff8}, {0x20ffc, 0x20200}, {0x20100, 0x21f00}}}) {
      convert(ticksFor(2000, 2, 29) + 123456789, input, output);
    }
    for (const uint32_t input : {0u, 0x21ffcu, 0x30000u, 0x50000u, 0xfffffffcu}) {
      prepare(ctx, input, 0x20200);
      reject(ctx, base);
    }
    for (const uint32_t output : {0u, 0x21ff2u, 0x30000u, 0x50000u, 0xfffffff8u}) {
      prepare(ctx, 0x20100, output);
      reject(ctx, base);
    }
    for (const uint32_t stack : {0x21ff8u, 0x30000u, 0x50000u, 0xfffffff8u}) {
      prepare(ctx, 0x20100, 0x20200);
      ctx.esp = stack;
      reject(ctx, base);
    }
    prepare(ctx, 0x20100, 0x20200);
    reject(ctx, nullptr);
    browser::Runtime foreign({}, {});
    auto alien = ctx;
    alien.host = &foreign;
    reject(alien, base);
    alien = ctx;
    alien.fs_base += 0x1000;
    reject(alien, base);
    {
      auto lease = image.memory->Borrow();
      reject(ctx, base);
    }
    {
      auto original = std::move(image.memory);
      image.memory = std::make_unique<browser::Memory>();
      image.memory->Ensure(0x20000, 8192);
      reject(ctx, base);
      image.memory = std::move(original);
    }
    auto* owner = image.boundRuntime;
    image.boundRuntime = &foreign;
    reject(ctx, base);
    image.boundRuntime = owner;
    image.importsBound = false;
    reject(ctx, base);
    image.importsBound = true;
    runtime.Cancel();
    reject(ctx, base);
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(!runtime.ActiveWorkers(), "time fields retained active workers");
  std::puts("time-fields-contracts-ok");
}
