#include "browser/thread-provider.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
std::atomic<bool> childEntered = false;
std::atomic<bool> parentReturned = false;
uint32_t mode = 0;
uint32_t parentKpcr = 0;
browser::PreparedImage* activeImage = nullptr;

const auto child = [](X86Context& ctx, uint8_t* base) {
  auto& memory = browser::Memory::From(base);
  auto& runtime = browser::Runtime::From(ctx);
  uint32_t argument = 0, handle = 0, id = 0, stackTop = 0, tls = 0, value = 0;
  memory.Read(ctx, ctx.esp + 4, &argument, 4);
  memory.Read(ctx, 0x10010, &handle, 4);
  memory.Read(ctx, 0x10014, &id, 4);
  memory.Read(ctx, ctx.fs_base + 4, &stackTop, 4);
  memory.Read(ctx, stackTop - 20, &tls, 4);
  memory.Read(ctx, tls, &value, 4);
  check(argument == 0x12345678 && handle && id == 0x14, "child entered before valid creation outputs");
  check(ctx.fs_base != parentKpcr && value == 0xaabbccdd, "child TLS or identity was not isolated");
  value = 0x76543210;
  memory.Write(ctx, tls, &value, 4);
  childEntered = true;
  while (!parentReturned.load()) {
    runtime.Checkpoint(ctx);
    std::this_thread::yield();
  }
  if (mode == 1) { runtime.Invoke(ctx, base, runtime.KernelTarget(349)); }
  if (mode == 2) { runtime.Cancel(); runtime.Checkpoint(ctx); }
  if (mode == 3) {
    runtime.Call(ctx, base, runtime.KernelTarget(258), {73});
    check(false, "thread termination returned to guest code");
  }
  ctx.eax = 42;
  ctx.esp += 8;
};
const auto systemRoutine = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  uint32_t start = 0, argument = 0;
  auto& memory = browser::Memory::From(base);
  memory.Read(ctx, ctx.esp + 4, &start, 4);
  memory.Read(ctx, ctx.esp + 8, &argument, 4);
  check(start == 0x11200, "system routine received wrong start routine");
  ctx.eax = runtime.Call(ctx, base, start, {argument});
  ctx.esp += 12;
};
const auto parent = [](X86Context& ctx, uint8_t* base) {
  parentKpcr = ctx.fs_base;
  auto& runtime = browser::Runtime::From(ctx);
  auto& memory = browser::Memory::From(base);
  const auto create = [&](uint32_t output, uint32_t extension, uint32_t tls, uint32_t suspended, uint32_t system) {
    return runtime.Call(ctx, base, runtime.KernelTarget(255), {output, extension, 0x10000, tls, 0x10014, 0x11200, 0x12345678, suspended, 0, system});
  };
  for (uint32_t n = 0; n < 5; ++n) {
    bool rejected = false;
    try { create(n == 0 ? 0x20000 : 0x10010, n == 1 ? 1 : 0, n == 2 ? 24 : 20, n == 3 ? 1 : 0, n == 4 ? 0x11900 : 0x11100); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected, "unsupported creation mode accepted");
    uint32_t output = 1;
    memory.Read(ctx, 0x10010, &output, 4);
    check(output == 0 && !childEntered, "failed creation published outputs or launched a child");
  }
  check(activeImage != nullptr, "thread fixture is absent");
  const size_t backing = memory.BackingBytes();
  bool rolledBack = false;
  try {
    browser::LaunchImageThread(*activeImage, runtime, ctx, 0x14, 0x10000,
      [&](const browser::ImageThread&) { throw browser::Fault(ctx, 0, 0, "refused admission"); });
  } catch (const browser::Fault&) { rolledBack = true; }
  check(rolledBack && activeImage->threads.size() == 1 && memory.BackingBytes() == backing,
    "failed child admission retained thread storage");
  check(create(0x10010, 0, 20, 0, mode == 4 ? 0 : 0x11100) == 0, "real child creation failed");
  uint32_t handle = 0;
  memory.Read(ctx, 0x10010, &handle, 4);
  check(runtime.Call(ctx, base, runtime.KernelTarget(187), {handle}) == 0, "thread handle close failed");
  check(runtime.Call(ctx, base, runtime.KernelTarget(187), {handle}) == 0xc0000008, "closed thread handle remained live");
  bool bounded = false;
  try { create(0x10018, 0, 20, 0, 0x11100); }
  catch (const browser::Fault&) { bounded = true; }
  check(bounded, "thread storage capacity was bypassed");
  if (mode == 5) {
    while (!childEntered.load()) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
    runtime.Cancel();
  }
  parentReturned = true;
  ctx.esp += 8;
};

int main() {
  for (uint32_t n = 0; n < 18; ++n) {
    mode = n % 6;
    childEntered = false;
    parentReturned = false;
    const std::array<X86FuncMapping, 3> mappings = {{{0x11000, parent}, {0x11100, systemRoutine}, {0x11200, child}}};
    browser::Runtime runtime(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Map(0x10000, 4096);
    image.memory->Commit(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    image.image.entry = 0x11000;
    image.image.tls_addr = 0x10080;
    image.image.tls.index_addr = 0x10084;
    image.tlsTemplate.resize(16);
    const uint32_t initial = 0xaabbccdd;
    std::memcpy(image.tlsTemplate.data(), &initial, 4);
    const uint32_t import = 0x8000015d;
    image.imports = {{0x10000, 349}};
    image.memory->Write({}, 0x10000, &import, 4);
    browser::InstallThreadProvider(runtime, image);
    browser::DeclareDiagnosticImport(image, runtime, 349, false);
    browser::BindImageImports(image, runtime, {});
    X86Context foreign{};
    foreign.host = &runtime;
    foreign.esp = 0x10800;
    bool rejected = false;
    try { runtime.Kernel(foreign, image.memory->Base(), 255); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && foreign.esp == 0x10800 && image.threads.empty(), "host caller created a session child");
    browser::Runtime other({}, {});
    foreign.host = &other;
    rejected = false;
    try { runtime.Kernel(foreign, image.memory->Base(), 255); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && image.threads.empty(), "foreign runtime created a session child");
    activeImage = &image;
    browser::ImageExecution execution;
    bool faulted = false;
    try { browser::ExecuteImageEntry(image, runtime, execution); }
    catch (const browser::UnsupportedImport& fault) {
      check(mode == 1 && fault.ordinal == 349 && fault.context->fs_base == image.threads[1].kpcr, "child fault lost identity");
      faulted = true;
    } catch (const browser::Cancelled&) {
      check(mode == 2 || mode == 5, "unexpected cancellation");
      faulted = true;
    }
    check(faulted == (mode == 1 || mode == 2 || mode == 5) && execution.entered && execution.returned == (mode != 5) && childEntered,
      "entry completed without owned child work");
    check(runtime.ActiveWorkers() == 0 && image.threads.size() == 2, "owned thread teardown incomplete");
    uint32_t parentTls = 0;
    image.memory->Read({}, image.threads[0].tlsData, &parentTls, 4);
    check(parentTls == initial, "child modified parent TLS");
  }
  std::puts("thread-provider-contracts-ok");
}
