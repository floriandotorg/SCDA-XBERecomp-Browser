#include "browser/menu-launch.h"
#include "cpu/x86_ops.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>

const auto check = [](bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } };
X86_FUNC(__imp__sub_00055666) { throw std::runtime_error("original launch executed"); }
X86_FUNC(__imp__sub_000530A7) { throw std::runtime_error("original utility mount executed"); }
X86_FUNC(sub_00055666);

int main() {
  const auto development = browser::DevelopmentIcelandLaunchPage();
  const auto launchWord = [&](uint32_t offset) { uint32_t value; std::memcpy(&value, development.data() + offset, 4); return value; };
  check(launchWord(0) == 0 && launchWord(4) == 0x5553005e, "development launch has the wrong title or type");
  check(std::strcmp(reinterpret_cast<const char*>(development.data() + 8), "\\DEVICE\\CDROM0;default.xbe") == 0, "development launch changed its executable");
  check(std::strcmp(reinterpret_cast<const char*>(development.data() + 540), "\\DEVICE\\CDROM0") == 0 &&
    std::strcmp(reinterpret_cast<const char*>(development.data() + 555), "default.xbe") == 0, "development launch overwrote its device path terminator");
  check(launchWord(1024) == 0x73636d6f && launchWord(1032) == 0x70616431 && launchWord(1040) == 0x7363336d && launchWord(1044) == 1 && launchWord(1048) == 0x73633370, "development launch lost original payload records");
  const auto wideText = [&](uint32_t offset) { std::u16string text; for (uint32_t n = offset; n + 1 < development.size(); n += 2) { const char16_t value = development[n] | (uint16_t(development[n + 1]) << 8); if (!value) { return text; } text.push_back(value); } throw std::runtime_error("unterminated launch text"); };
  check(wideText(1060) == u"SAMFISHER", "development launch lost the temporary player name");
  check(wideText(1104) == u"MAPNAME=x01_Iceland_a?MENUPAGE=2?PLAYER1=SAMFISHER?MISSIONMODE=1?MISSIONDIFF=1?TRUSTVALUE=75 PLAYERNAME=SAMFISHER TRAVELDATA=FALSE  NEWGAME=TRUE LastScreen=D:\\Default\\DataXb\\Screens\\int\\x01_Iceland_a_loading.tga", "development launch changed the observed New Game request");
  check(std::all_of(development.begin() + 1528, development.end(), [](uint8_t value) { return value == 0; }), "development launch contains uninitialized tail data");
  const std::array<X86FuncMapping, 1> mappings = {{{0x55666, sub_00055666}}};
  {
    browser::Runtime runtime(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.memory->Ensure(0x20000, 8192);
    browser::MenuLaunch launch(runtime, image);
    launch.ConfigureDevelopmentStart();
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
      ctx.esp = 0xfffffffcu;
      bool mountRejected = false;
      try { launch.Dispatch(ctx, base, 136); } catch (const browser::Fault&) { mountRejected = true; }
      check(mountRejected && ctx.esp == 0xfffffffcu, "invalid development mount frame changed guest state");
      for (uint32_t n = 0; n < 2; ++n) {
        ctx.esp = 0x20f00;
        const std::array<uint32_t, 2> frame{0x53000, 0};
        image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
        check(launch.Dispatch(ctx, base, 136) && image.memory->Load<uint32_t>(ctx, ctx.esp + 4) == (n ? 0u : 1u), "development utility mount did not initialize exactly once through the original function");
      }
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
      const std::string longest = "D:\\" + std::string(245, 'a') + "\\default.xbe";
      check(longest.size() == 260, "path fixture length");
      caught = false;
      try { invoke(longest.c_str(), 0); }
      catch (const browser::MenuLaunchRequested& request) { caught = request.path == longest && !request.hasData; }
      check(caught, "host-compatible path limit rejected");
      for (const std::string path : {"a" + longest, std::string("D:\\bad\n\\default.xbe"), std::string("D:\\bad\x80\\default.xbe")}) {
        bool rejected = false;
        try { invoke(path.c_str(), 0); } catch (const browser::Fault&) { rejected = true; }
        catch (const browser::MenuLaunchRequested&) {}
        check(rejected && ctx.esp == 0x20f00 && ctx.eax == 0xabcdef01, "host-incompatible path escaped native validation");
      }
      bool fault = false;
      try { invoke("default.xbe", 0x21fff); } catch (const browser::Fault&) { fault = true; }
      check(fault, "invalid payload accepted");
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
  {
    browser::Runtime originalRuntime(mappings, {});
    browser::PreparedImage originalImage;
    originalImage.memory = std::make_unique<browser::Memory>();
    originalImage.memory->Ensure(0x10000, 4096);
    originalImage.memory->Ensure(0x20000, 8192);
    browser::MenuLaunch originalLaunch(originalRuntime, originalImage, true);
    originalImage.memory->Ensure(0x30000, 8192);
    originalLaunch.InstallFirmware(0x30000);
    originalImage.image.kernel_thunk = 0x10000;
    originalImage.memory->Write({}, 0x10000, std::array<uint32_t, 1>{0}.data(), 4);
    browser::BindImageImports(originalImage, originalRuntime, {});
    X86Context originalContext{};
    originalContext.host = &originalRuntime;
    originalRuntime.Launch(originalContext, originalImage.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      ctx.esp = 0x20f00;
      originalImage.memory->Store(ctx, ctx.esp + 4, 0u);
      check(originalLaunch.Dispatch(ctx, base, 136) && originalImage.memory->Load<uint32_t>(ctx, ctx.esp + 4) == 0, "normal utility mount was changed by development support");
      constexpr char path[] = "D:\\default.xbe";
      originalImage.memory->Write(ctx, 0x20100, path, sizeof(path));
      const std::array<uint32_t, 3> frame = {0x12345, 0x20100, 0x21000};
      ctx.esp = 0x20f00;
      originalImage.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      check(originalLaunch.Dispatch(ctx, base, 64), "reboot mode bypassed original launch page construction");
      check(ctx.esp == 0x20f00, "original delegation changed launch ABI");
      std::array<uint8_t, 4096> page{};
      page[0] = 1;
      constexpr char imagePath[] = "\\Device\\CdRom0;default.xbe";
      std::memcpy(page.data() + 8, imagePath, sizeof(imagePath));
      page[1024] = 41;
      page[4095] = 93;
      const uint32_t pageAddress = 0x31000;
      originalImage.memory->Write(ctx, pageAddress, page.data(), page.size());
      originalImage.memory->Write(ctx, 0x30000, &pageAddress, 4);
      const std::array<uint32_t, 2> firmwareFrame = {0x55829, 2};
      originalImage.memory->Write(ctx, ctx.esp, firmwareFrame.data(), sizeof(firmwareFrame));
      bool captured = false;
      try { originalRuntime.Invoke(ctx, base, originalRuntime.KernelTarget(49)); }
      catch (const browser::MenuLaunchRequested& request) {
        captured = request.page && *request.page == page && request.path == imagePath && request.hasData &&
          request.data.front() == 41 && request.data.back() == 93;
      }
      check(captured, "firmware reboot did not own the complete original launch page");
      page[1024] = 0;
      originalImage.memory->Write(ctx, pageAddress, page.data(), page.size());
      for (const uint32_t mode : {0u, 1u, 3u, 0xffffffffu}) {
        const std::array<uint32_t, 2> invalidFrame = {0x55829, mode};
        originalImage.memory->Write(ctx, ctx.esp, invalidFrame.data(), sizeof(invalidFrame));
        bool rejected = false;
        try { originalRuntime.Invoke(ctx, base, originalRuntime.KernelTarget(49)); }
        catch (const browser::Fault&) { rejected = true; }
        check(rejected, "unsupported firmware mode relaunched a guest");
      }
    });
    if (const auto failure = originalRuntime.Join()) { std::rethrow_exception(failure); }
  }
  std::puts("menu-launch-contracts-ok");
}
