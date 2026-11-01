#include "browser/menu-launch.h"
#include "cpu/x86_ops.h"

#include <array>
#include <cstdio>
#include <cstring>

const auto check = [](bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } };
X86_FUNC(__imp__sub_00055666) { throw std::runtime_error("original launch executed"); }
X86_FUNC(sub_00055666);

int main() {
  const std::array<X86FuncMapping, 1> mappings = {{{0x55666, sub_00055666}}};
  browser::Runtime runtime(mappings, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 4096);
  image.memory->Ensure(0x20000, 8192);
  browser::MenuLaunch launch(runtime, image);
  image.image.kernel_thunk = 0x10000;
  image.memory->Write({}, 0x10000, std::array<uint32_t, 1>{0}.data(), 4);
  browser::BindImageImports(image, runtime, {});
  X86Context context{};
  context.host = &runtime;
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto invoke = [&](const char* path, uint32_t data) {
      if (path) { image.memory->Write(ctx, 0x20100, path, std::strlen(path) + 1); }
      const std::array<uint32_t, 3> frame = {0x12345, path ? 0x20100u : 0u, data};
      ctx.esp = 0x20f00;
      ctx.eax = 0xabcdef01;
      image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      return launch.Dispatch(ctx, base, 64);
    };
    for (const char* path : {static_cast<const char*>(nullptr), "", "D:\\sc4_adv.xbe", "D:\\other.xbe", "xdefault.xbe"}) {
      check(!invoke(path, 0), "unsupported launch delegated");
      check(ctx.eax == 2 && ctx.esp == 0x20f0c, "rejected launch ABI");
    }
    std::array<uint8_t, 3072> payload{};
    payload[0] = 42; payload[3071] = 99;
    image.memory->Write(ctx, 0x21000, payload.data(), payload.size());
    bool caught = false;
    try { invoke("D:\\DEFAULT.XBE", 0x21000); }
    catch (const browser::MenuLaunchRequested& request) {
      caught = request.path == "D:\\DEFAULT.XBE" && request.hasData && request.data == payload;
    }
    check(caught && ctx.esp == 0x20f00 && ctx.eax == 0xabcdef01, "story launch did not preserve owned request");
    caught = false;
    try { invoke("default.xbe", 0); }
    catch (const browser::MenuLaunchRequested& request) { caught = !request.hasData; }
    check(caught, "null data launch lost");
    bool fault = false;
    try { invoke("default.xbe", 0x21fff); } catch (const browser::Fault&) { fault = true; }
    check(fault, "invalid payload accepted");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  std::puts("menu-launch-contracts-ok");
}
