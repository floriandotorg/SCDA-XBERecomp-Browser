#include "browser/menu-input.h"
#include "cpu/x86_ops.h"
#include <algorithm>
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto rejects = [](const auto& action) {
  bool rejected = false;
  try { action(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, "unsafe input operation accepted");
};
X86_FUNC(__imp__sub_000534B8) { throw browser::Fault(ctx, 0x534b8, 0, "signature Begin is absent from input fixture"); }
X86_FUNC(__imp__sub_0005351A) { throw browser::Fault(ctx, 0x5351a, 0, "signature End is absent from input fixture"); }
std::atomic<uint32_t> originals = 0;
#define ORIGINAL(name, bytes) \
  X86_FUNC(__imp__sub_##name) { ++originals; ctx.esp += bytes; } \
  X86_WEAK_FUNC(sub_##name, __imp__sub_##name)
ORIGINAL(00F11000, 8)
ORIGINAL(00F12000, 12)
ORIGINAL(00F13000, 28)
ORIGINAL(004ECDE8, 12)
ORIGINAL(004EDB24, 20)
ORIGINAL(004EDB7A, 8)
ORIGINAL(004EDB86, 12)
ORIGINAL(004EDBF9, 12)
X86_FUNC(sub_004ECDE8);
X86_FUNC(sub_004EDB24);
X86_FUNC(sub_004EDB7A);
X86_FUNC(sub_004EDB86);
X86_FUNC(sub_004EDBF9);

class InputRoute final : public browser::SdkGraphics {
  std::shared_ptr<browser::MenuInput> input_;
public:
  bool delegate = false;
  explicit InputRoute(std::shared_ptr<browser::MenuInput> input) : input_(std::move(input)) {}
  bool BelongsTo(browser::Runtime& runtime, browser::PreparedImage& image) const override { return input_->BelongsTo(runtime, image); }
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override {
    return delegate || input_->Dispatch(ctx, base, id);
  }
};

int contracts() {
  const std::array<X86FuncMapping, 8> mappings{{{0x4ecde8, sub_004ECDE8}, {0x4edb24, sub_004EDB24},
    {0x4edb7a, sub_004EDB7A}, {0x4edb86, sub_004EDB86}, {0x4edbf9, sub_004EDBF9},
    {0xf11000, sub_00F11000}, {0xf12000, sub_00F12000}, {0xf13000, sub_00F13000}}};
  browser::Runtime runtime(mappings, {});
  browser::Runtime foreign({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  auto& memory = *image.memory;
  memory.Ensure(0x10000, 0x1000);
  memory.Ensure(0x20000, 0x4000);
  memory.Ensure(0x4ec000, 0x1000);
  memory.Ensure(0x80001000, 0x1000);
  image.image.kernel_thunk = 0x10000;
  auto gamepadStorage = std::make_shared<browser::MenuInput::GamepadStorage>();
  auto input = std::make_shared<browser::MenuInput>(runtime, image, std::shared_ptr<browser::MenuInput::SharedStorage>{}, gamepadStorage);
  check(input->BelongsTo(runtime, image) && !input->BelongsTo(foreign, image), "input ownership identity missing");
  check(input->SharedWords() == 2 && input->SharedAddress() != nullptr, "input shared state contract changed");
  auto route = std::make_shared<InputRoute>(input);
  const auto sdk = browser::InstallSdkProvider(runtime, image, browser::SdkPolicy::UnavailableXNet, {}, route);
  check(bool(sdk), "SDK input injection failed");
  browser::BindImageImports(image, runtime, {});
  rejects([&] { browser::MenuInput late(runtime, image); });
  X86Context context{};
  context.host = &runtime;
  context.fs_base = 0x12340000;
  context.esp = 0x22000;
  context.ebx = 0x11223344;
  rejects([&] { input->Dispatch(context, memory.Base(), 40); });
  const auto state = input->SharedAddress();
  const auto gamepad = input->GamepadAddress();
  check(input->GamepadWords() == 8 && gamepad == gamepadStorage->data(), "gamepad publication lost its owned storage");
  runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto call = [&](X86Func* function, std::initializer_list<uint32_t> args) {
      std::vector<uint32_t> frame{0xabcdef};
      frame.insert(frame.end(), args);
      ctx.esp = 0x22000;
      memory.Write(ctx, ctx.esp, frame.data(), frame.size() * 4);
      function(ctx, base);
      check(ctx.esp == 0x22000 + frame.size() * 4 && ctx.ebx == context.ebx, "input stdcall frame or nonvolatile register damaged");
      return ctx.eax;
    };
    {
      auto lease = memory.Borrow();
      rejects([&] { input->Dispatch(ctx, base, 40); });
    }
    rejects([&] { input->Dispatch(ctx, nullptr, 40); });
    auto impostor = ctx;
    impostor.host = &foreign;
    rejects([&] { input->Dispatch(impostor, base, 40); });
    impostor = ctx;
    ++impostor.fs_base;
    rejects([&] { input->Dispatch(impostor, base, 40); });
    check(call(sub_004ECDE8, {0, 0}) == 0, "device initialization failed");
    std::array<uint32_t, 3> devices;
    memory.Read(ctx, 0x4ec4d4, devices.data(), sizeof(devices));
    check(devices == std::array<uint32_t, 3>{1, 1, 0}, "single keyboard controller connection unavailable");
    check(call(sub_004EDB24, {0, 0, 0, 0}) == 0, "wrong device type accepted");
    check(call(sub_004EDB24, {0x4ec4d4, 4, 0, 0}) == 0, "invalid port accepted");
    const uint32_t handle = call(sub_004EDB24, {0x4ec4d4, 0, 0, 0});
    check(handle != 0, "controller did not open");
    const uint32_t disconnected = call(sub_004EDB24, {0x4ec4d4, 1, 0, 0});
    check(call(sub_004EDB86, {disconnected, 0x20000}) == 1167, "unconnected port fabricated a second player");
    const auto read = [&] {
      check(call(sub_004EDB86, {handle, 0x20000}) == 0, "state read failed");
      std::array<uint8_t, 22> bytes;
      memory.Read(ctx, 0x20000, bytes.data(), bytes.size());
      return bytes;
    };
    auto bytes = read();
    check(bytes[0] == 1 && bytes[4] == 0 && bytes[6] == 0, "initial connected packet incorrect");
    state[0].store(browser::MenuInput::Up | browser::MenuInput::Right | browser::MenuInput::Forward |
      browser::MenuInput::StrafeLeft | browser::MenuInput::Start | browser::MenuInput::Confirm | browser::MenuInput::Back);
    bytes = read();
    check(bytes[0] == 2 && bytes[4] == 0x19 && bytes[6] == 255 && bytes[7] == 255,
      "digital buttons or analog A/B guest offsets incorrect");
    check(bytes[14] == 1 && bytes[15] == 0x80 && bytes[16] == 255 && bytes[17] == 0x7f &&
      bytes[18] == 0 && bytes[19] == 0 && bytes[20] == 0 && bytes[21] == 0, "guest signed sticks incorrect");
    state[1].fetch_add(1);
    check(read() == bytes, "repeat revision fabricated a guest packet");
    state[0].store(0);
    check(read()[0] == 3 && read()[0] == 3, "release did not settle without a queued burst");
    state[0].store(browser::MenuInput::Forward | browser::MenuInput::Backward | browser::MenuInput::StrafeLeft | browser::MenuInput::StrafeRight);
    bytes = read();
    check(bytes[0] == 3 && bytes[14] == 0 && bytes[15] == 0 && bytes[16] == 0 && bytes[17] == 0,
      "opposing keys did not cancel to unchanged neutral state");
    const std::array<uint32_t, 6> analogMasks{2048, 4096, 8192, 16384, 32768, 65536};
    for (uint32_t n = 0; n < analogMasks.size(); ++n) {
      state[0].store(analogMasks[n]);
      bytes = read();
      std::array<uint8_t, 8> expected{};
      expected[n + 2] = 255;
      check(std::equal(expected.begin(), expected.end(), bytes.begin() + 6), "profile keyboard analog button guest offset incorrect");
      check(bytes[4] == 0 && bytes[5] == 0, "analog profile action became a digital button");
    }
    for (uint32_t n = 0; n < 3; ++n) {
      state[0].store(131072u << n);
      bytes = read();
      check(bytes[4] == (0x20u << n) && bytes[5] == 0,
        "OPSAT and stick click actions must reach their original digital button bits");
      check(std::all_of(bytes.begin() + 6, bytes.end(), [](uint8_t value) { return value == 0; }),
        "digital mission action changed analog buttons or sticks");
    }
    state[0].store(0);
    read();
    gamepad[0].store(1);
    gamepad[1].store(1);
    gamepad[2].store(browser::MenuInput::Start | browser::MenuInput::Confirm | browser::MenuInput::RightThumb);
    gamepad[3].store(16384);
    gamepad[4].store(uint32_t(-8192));
    gamepad[5].store(uint32_t(-32767));
    gamepad[6].store(32767);
    gamepad[0].store(2);
    bytes = read();
    check(bytes[4] == 0x90 && bytes[6] == 255 && bytes[14] == 0 && bytes[15] == 0x40 && bytes[16] == 0 && bytes[17] == 0xe0 &&
      bytes[18] == 1 && bytes[19] == 0x80 && bytes[20] == 255 && bytes[21] == 0x7f, "gamepad axes or original button offsets changed");
    gamepad[0].store(4);
    check(read() == bytes, "unchanged gamepad publication fabricated a controller packet");
    state[0].store(browser::MenuInput::Up | browser::MenuInput::Backward);
    bytes = read();
    check(bytes[4] == 0x91 && bytes[14] == 0 && bytes[15] == 0x40 && bytes[16] == 1 && bytes[17] == 0x80,
      "keyboard and gamepad did not merge buttons or select keyboard axes independently");
    state[0].store(0);
    gamepad[0].store(5);
    bytes = read();
    check(std::all_of(bytes.begin() + 4, bytes.end(), [](uint8_t value) { return value == 0; }), "partial gamepad publication retained held input");
    gamepad[0].store(6);
    check(read()[6] == 255, "completed gamepad publication did not resume input");
    gamepad[1].store(0);
    bytes = read();
    check(std::all_of(bytes.begin() + 4, bytes.end(), [](uint8_t value) { return value == 0; }), "gamepad disconnect retained axes or buttons");
    const uint32_t sentinel = 0xdeadbeef;
    memory.Write(ctx, 0x20000, &sentinel, 4);
    check(call(sub_004EDB86, {0x123456, 0x20000}) == 6, "invalid handle accepted");
    uint32_t unchanged;
    memory.Read(ctx, 0x20000, &unchanged, 4);
    check(unchanged == sentinel, "invalid handle changed output");
    const auto unsafeState = [&](uint32_t frameAddress, uint32_t output) {
      const std::array<uint32_t, 3> frame{0xabcdef, handle, output};
      ctx.esp = frameAddress;
      memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      const auto before = ctx;
      rejects([&] { sub_004EDB86(ctx, base); });
      check(ctx.esp == before.esp && ctx.eax == before.eax, "rejected state mutated CPU ABI");
      std::array<uint32_t, 3> after;
      memory.Read(ctx, frameAddress, after.data(), sizeof(after));
      check(after == frame, "rejected state corrupted input frame");
    };
    unsafeState(0x22000, 0x22004);
    unsafeState(0x80001000, 0xf0001004);
    unsafeState(0x22000, 0x23ff0);
    unsafeState(0x22000, 0xfffffff8);
    unsafeState(0x22000, 0x4ec4d4);
    unsafeState(0x22000, 0);
    state[0].store(0x80000000);
    unsafeState(0x22000, 0x20000);
    state[0].store(0);
    gamepad[1].store(1);
    gamepad[3].store(32768);
    unsafeState(0x22000, 0x20000);
    gamepad[3].store(0);
    gamepad[2].store(0x80000000);
    unsafeState(0x22000, 0x20000);
    gamepad[2].store(0);
    gamepad[7].store(1);
    unsafeState(0x22000, 0x20000);
    gamepad[7].store(0);
    gamepad[1].store(0);
    ctx.esp = 0xfffffff8;
    rejects([&] { input->Dispatch(ctx, base, 43); });
    check(ctx.esp == 0xfffffff8, "overflowing input cleanup changed stack");
    std::array<uint8_t, 70> feedback{};
    memory.Write(ctx, 0x20100, feedback.data(), feedback.size());
    check(call(sub_004EDBF9, {handle, 0x20100}) == 997, "eventless keyboard feedback ABI incorrect");
    const uint32_t event = 0x1234;
    memory.Write(ctx, 0x20104, &event, 4);
    check(call(sub_004EDBF9, {handle, 0x20100}) == 50, "unsupported feedback event falsely completed");
    check(call(sub_004EDB7A, {handle}) == 0 && call(sub_004EDB86, {handle, 0x20000}) == 6, "closed controller handle remained live");
    const uint32_t replacement = call(sub_004EDB24, {0x4ec4d4, 0, 0, 0});
    check(replacement != handle, "closed input handle was recycled");
    std::vector<uint32_t> opened{replacement, disconnected};
    for (uint32_t n = 0; n < 30; ++n) { opened.push_back(call(sub_004EDB24, {0x4ec4d4, 0, 0, 0})); }
    check(opened.back() && call(sub_004EDB24, {0x4ec4d4, 0, 0, 0}) == 0, "controller handle budget unbounded");
    route->delegate = true;
    call(sub_004EDB7A, {0});
    check(originals == 1, "retained original alias unavailable");
    route->delegate = false;
    state[0].store(browser::MenuInput::Confirm);
    ctx.esp = 0x22000;
    const std::array<uint32_t, 3> frame{0xabcdef, replacement, 0x20000};
    memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    runtime.Cancel();
    bool cancelled = false;
    try { sub_004EDB86(ctx, base); } catch (const browser::Cancelled&) { cancelled = true; }
    memory.Read(ctx, 0x20000, &unchanged, 4);
    check(cancelled && unchanged == sentinel && ctx.esp == 0x22000, "cancelled input changed guest state or lost cancellation");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  runtime.Stop();
  std::puts("menu-input-contracts-ok");
  return 0;
}
int main() {
  try { return contracts(); } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
