#include "browser/file-provider.h"
#include "browser/thread-provider.h"
#include "browser/temporary-filesystem.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
std::function<bool(uint32_t)> closeFile;
const auto entry = [](X86Context& ctx, uint8_t* base) {
  auto& memory = browser::Memory::From(base);
  auto& runtime = browser::Runtime::From(ctx);
  const auto close = [&](uint32_t handle) { return runtime.Call(ctx, base, runtime.KernelTarget(187), {handle}) == 0; };
  const auto put = [&](uint32_t at, uint32_t value) { memory.Write(ctx, at, &value, 4); };
  const auto get = [&](uint32_t at) { uint32_t value; memory.Read(ctx, at, &value, 4); return value; };
  const auto open = [&](std::string path, uint32_t root = 0, uint32_t access = 0x100001, uint32_t options = 0x20, uint32_t output = 0x10100, uint32_t status = 0x10108, uint32_t share = 3) {
    memory.Write(ctx, 0x10300, path.data(), path.size());
    put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16));
    put(0x10204, 0x10300);
    put(0x10210, root); put(0x10214, 0x10200); put(0x10218, 0x40);
    put(0x10100, 0xabcdef); put(0x10108, 0xabcdef); put(0x1010c, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(202), {output, access, 0x10210, status, share, options});
    check(ctx.esp == stack, "open stdcall cleanup corrupted stack");
    check(get(0x10108) == result && get(0x1010c) == (result ? 0u : 1u), "open status is not truthful");
    if (result) { check(get(0x10100) == 0xabcdef, "failed open published a handle"); }
    return result;
  };
  const auto openExisting = [&](std::string path, uint32_t options = 0x60, uint32_t share = 1, uint32_t root = 0xfffffffdu,
      uint32_t access = 0x80100080, uint32_t disposition = 1, uint32_t attributes = 0x80, uint32_t allocation = 0,
      uint32_t output = 0x10100, uint32_t status = 0x10108) {
    memory.Write(ctx, 0x10300, path.data(), path.size());
    put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(0x10204, 0x10300);
    put(0x10210, root); put(0x10214, 0x10200); put(0x10218, 0x40);
    put(0x10100, 0xabcdef); put(0x10108, 0xabcdef); put(0x1010c, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(190), {output, access, 0x10210, status, allocation, attributes, share, disposition, options});
    const auto information = get(status + 4);
    check(ctx.esp == stack && get(status) == result && (result ? information == 0 : disposition == 5 ? information == 2 || information == 3 : information == 1),
      "create-file status or cleanup corrupted");
    if (result) { check(get(0x10100) == 0xabcdef, "failed OPEN_EXISTING published a handle"); }
    return result;
  };
  for (uint32_t options : {0x60u, 0x64u}) {
    for (uint32_t share : {1u, 7u}) {
      check(openExisting("d:\\DEFAULT.XBE", options, share) == 0, "observed synchronous OPEN_EXISTING refused");
      const auto handle = get(0x10100);
      for (uint32_t n = 0; n < 2; ++n) {
        put(0x10900, 0xabcdef); put(0x10840, 0xabcdef); put(0x10844, 0xabcdef);
        check(runtime.Call(ctx, base, runtime.KernelTarget(219), {handle, 0, 0, 0, 0x10840, 0x10900, 3, 0}) == 0 &&
          get(0x10840) == 0 && get(0x10844) == 3 && (get(0x10900) & 0xffffff) == (n ? 0x333231u : 0x4f5349u),
          "ordinal190 disc handle lost its source or synchronous position");
      }
      check(close(handle) && !close(handle), "OPEN_EXISTING handle lifetime broken");
    }
  }
  for (const std::string path : {"\\??\\d:\\default.xbe", "\\device\\cdrom0\\default.xbe"}) {
    check(openExisting(path) == 0 && close(get(0x10100)), "pseudo-root namespace alias or case folding failed");
    check(openExisting(path, 0x64, 7, 0) == 0 && close(get(0x10100)), "ordinary absolute OPEN_EXISTING failed");
  }
  check(openExisting("default.xbe") == 0xc0000033, "pseudo-root admitted relative path");
  check(openExisting("D:default.xbe") == 0xc0000033, "pseudo-root admitted drive-relative path");
  check(openExisting("D:\\..\\default.xbe") == 0xc0000033, "pseudo-root admitted traversal");
  for (uint32_t root : {0xffffffffu, 0xfffffffeu, 0xfffffffcu, 0x70000004u}) {
    check(openExisting("D:\\default.xbe", 0x60, 1, root) == 0xc0000008, "invalid root silently became namespace root");
  }
  for (uint32_t options : {0u, 4u, 0x40u, 0x50u, 0x70u, 0x62u, 0x1060u, 0x2060u, 0x8060u}) {
    check(openExisting("D:\\default.xbe", options) == 0xc00000bb, "unobserved OPEN_EXISTING mode accepted");
  }
  for (uint32_t disposition : {0u, 2u, 3u, 4u, 5u}) {
    check(openExisting("D:\\default.xbe", 0x60, 1, 0xfffffffdu, 0x80100080, disposition) == 0xc00000bb,
      "read OPEN_EXISTING broadened create or overwrite dispositions");
  }
  check(openExisting("D:\\default.xbe", 0x60, 0) == 0, "exclusive OPEN_EXISTING refused");
  const auto exclusiveExisting = get(0x10100);
  check(openExisting("D:\\default.xbe", 0x64, 7) == 0xc0000043, "share-delete bypassed existing read exclusion");
  check(close(exclusiveExisting), "exclusive OPEN_EXISTING did not close");
  check(openExisting("D:\\default.xbe", 0x64, 7) == 0, "shared OPEN_EXISTING refused");
  const auto sharedExisting = get(0x10100);
  check(openExisting("D:\\default.xbe", 0x60, 0) == 0xc0000043, "exclusive OPEN_EXISTING ignored existing reader");
  check(close(sharedExisting), "shared OPEN_EXISTING did not close");
  check(openExisting("D:\\default.xbe", 0x60, 1, 0xfffffffdu, 1) == 0xc000000d, "synchronous open admitted missing synchronize access");
  check(openExisting("D:\\default.xbe", 0x60, 1, 0xfffffffdu, 0x80100080, 1, 0x10) == 0xc000000d,
    "non-directory OPEN_EXISTING accepted directory attributes");
  check(openExisting("D:\\default.xbe", 0x60, 8) == 0xc00000bb, "unknown share bit accepted");
  check(openExisting("D:\\default.xbe", 0x60, 1, 0xfffffffdu, 0xc0100080) == 0xc00000bb, "new read mode admitted writes");
  check(openExisting("D:\\default.xbe", 0x60, 1, 0xfffffffdu, 0x80110080) == 0xc00000bb, "share-delete granted DELETE access");
  check(openExisting("D:\\", 0x60) == 0xc00000ba, "OPEN_EXISTING non-directory opened disc root");
  check(openExisting("D:\\absent") == 0xc0000034 && openExisting("D:\\absent\\child") == 0xc000003a,
    "OPEN_EXISTING missing disc path status fabricated");
  for (uint32_t n = 0; n < 5; ++n) {
    bool refused = false;
    try { openExisting("D:\\default.xbe", 0x60, 1, 0xfffffffdu, 0x80100080, 1, 0x80, 0,
      n == 0 ? 0x20000 : n == 1 ? 0x10108 : 0x10100, n == 2 ? 0x20000 : n == 3 ? 0x10100 : n == 4 ? 0x10102 : 0x10108); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10100) == 0xabcdef && get(0x10108) == 0xabcdef && get(0x1010c) == 0xabcdef,
      "invalid OPEN_EXISTING outputs partially published");
  }
  const auto createAlias = [&](std::string name, std::string target) {
    memory.Write(ctx, 0x10500, name.data(), name.size());
    memory.Write(ctx, 0x10600, target.data(), target.size());
    put(0x10440, uint32_t(name.size()) | (uint32_t(name.size()) << 16)); put(0x10444, 0x10500);
    put(0x10448, uint32_t(target.size()) | (uint32_t(target.size()) << 16)); put(0x1044c, 0x10600);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(67), {0x10440, 0x10448});
    check(ctx.esp == stack, "alias stdcall cleanup corrupted stack");
    return result;
  };
  check(open("\\Device\\Harddisk0\\Partition3", 0, 0x100003, 0x18, 0x10100, 0x10108, 0) == 0, "observed exclusive cache formatter open refused");
  auto formatter = get(0x10100);
  check(open("\\Device\\Harddisk0\\Partition3", 0, 0x100003, 0x18, 0x10100, 0x10108, 0) == 0xc0000043, "cache formatter lease not exclusive");
  check(open("\\Device\\Harddisk0\\Partition3\\", 0, 0x100001, 0x800021) == 0xc00000a3, "unformatted cache namespace exposed");
  const auto deviceControl = [&](uint32_t code, uint32_t length, uint32_t output = 0x10a00, uint32_t status = 0x10840, uint32_t event = 0, uint32_t handle = 0) {
    put(0x10840, 0xabcdef); put(0x10844, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(196), {handle ? handle : formatter, event, 0, 0, status, code, 0, 0, output, length});
    check(ctx.esp == stack && get(status) == result && get(status + 4) == (result ? 0 : code == 0x70000 ? 24 : 32), "raw IOCTL status or frame cleanup corrupted");
    return result;
  };
  check(deviceControl(0x70000, 24) == 0 && get(0x10a00) == 1024 && get(0x10a04) == 0 &&
    get(0x10a08) == 12 && get(0x10a0c) == 1 && get(0x10a10) == 32 && get(0x10a14) == 512, "virtual cache geometry not coherent");
  check(deviceControl(0x74004, 32) == 0 && get(0x10a00) == 0 && get(0x10a04) == 0 &&
    get(0x10a08) == 16 * 1024 * 1024 && get(0x10a0c) == 0 && get(0x10a10) == 0 &&
    get(0x10a14) == 3 && get(0x10a18) == 0 && get(0x10a1c) == 0, "virtual partition record not backed by owned cache");
  put(0x10a00, 0x12345678);
  check(deviceControl(0x70000, 23) == 0xc0000023 && get(0x10a00) == 0x12345678, "short geometry query modified output");
  check(deviceControl(0x74004, 31) == 0xc0000023 && get(0x10a00) == 0x12345678, "short partition query modified output");
  check(deviceControl(0x1234, 24) == 0xc00000bb && get(0x10a00) == 0x12345678, "unknown IOCTL faked success");
  check(deviceControl(0x70000, 24, 0x10a00, 0x10840, 1) == 0xc00000bb, "async IOCTL accepted");
  for (uint32_t n = 0; n < 3; ++n) {
    bool refused = false;
    try { deviceControl(0x70000, 24, n == 0 ? 0x20000 : n == 1 ? 0x10840 : 0x10a00, n == 2 ? 0x10a04 : 0x10840); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10840) == 0xabcdef && get(0x10a00) == 0x12345678, "invalid IOCTL partially published output");
  }
  const auto rawPartitionTransfer = [&](uint32_t ordinal, uint64_t offset, uint32_t length = 4096, uint32_t buffer = 0x30000, uint32_t status = 0x10840) {
    put(0x10700, uint32_t(offset)); put(0x10704, uint32_t(offset >> 32));
    put(0x10840, 0xabcdef); put(0x10844, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(ordinal), {formatter, 0, 0, 0, status, buffer, length, 0x10700});
    check(ctx.esp == stack && get(status) == result && get(status + 4) == (result ? 0 : length), "cache partition transfer status or cleanup corrupted");
    return result;
  };
  check(rawPartitionTransfer(219, 0) == 0xc00000bb, "unobserved raw cache partition read enabled");
  std::array<uint8_t, 4096> rawBytes;
  rawBytes.fill(0x63);
  memory.Write(ctx, 0x30000, rawBytes.data(), rawBytes.size());
  check(rawPartitionTransfer(236, 0) == 0, "observed cache partition explicit write refused");
  for (uint32_t ordinal : {236u}) {
    check(rawPartitionTransfer(ordinal, 1) == 0xc000000d, "cache partition accepted unaligned offset");
    check(rawPartitionTransfer(ordinal, 0, 511) == 0xc000000d, "cache partition accepted unaligned length");
    check(rawPartitionTransfer(ordinal, 16 * 1024 * 1024 - 512, 1024) == 0xc000000d, "cache partition admitted overflow beyond capacity");
    check(rawPartitionTransfer(ordinal, UINT64_MAX) == 0xc000000d, "cache partition admitted negative offset");
    bool refused = false;
    try { rawPartitionTransfer(ordinal, 0, 4096, 0x30000, 0x30004); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10840) == 0xabcdef && get(0x30000) == 0x63636363, "cache partition invalid output partially published");
  }
  check(get(0x30000) == 0x63636363, "cache raw write changed input buffer");
  check(close(formatter) && !close(formatter), "cache formatter lease did not close");
  check(deviceControl(0x70000, 24) == 0xc0000008, "closed formatter IOCTL remained usable");
  check(open("\\Device\\Harddisk0\\Partition0", 0, 0xc0100000, 0x10) == 0, "observed owned raw cache open refused");
  const auto raw = get(0x10100);
  check(open("\\Device\\Harddisk0\\Partition0", 0, 0xc0100000, 0x10, 0x10100, 0x10108, 0) == 0xc0000043, "raw cache sharing bypassed");
  check(open("child", raw) == 0xc0000024, "raw cache used as directory root");
  check(open("\\Device\\Harddisk0\\Partition0\\child", 0, 0xc0100000, 0x10) == 0xc00000bb, "raw cache admitted child path");
  check(open("\\Device\\Harddisk0\\Partition0", 0, 0xc0100000, 0x20) == 0xc00000bb, "unobserved raw cache open mode accepted");
  const auto transfer = [&](uint32_t ordinal, uint32_t handle = 0, uint32_t length = 512, uint32_t buffer = 0x10a00, uint32_t offset = 0x10700, uint32_t event = 0, uint32_t status = 0x10840) {
    put(0x10840, 0xabcdef); put(0x10844, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(ordinal), {handle ? handle : raw, event, 0, 0, status, buffer, length, offset});
    check(ctx.esp == stack && get(status) == result && get(status + 4) == (result ? 0 : length), "raw transfer status or stdcall cleanup corrupted");
    return result;
  };
  put(0x10700, 0x800); put(0x10704, 0);
  check(transfer(219) == 0 && get(0x10a00) == 0 && get(0x10bfc) == 0, "fresh raw cache did not read zero sector");
  for (uint32_t n = 0; n < 128; ++n) { put(0x10a00 + n * 4, 0x12345678); }
  check(transfer(236) == 0, "explicit raw sector write refused");
  put(0x10a00, 0);
  check(transfer(219) == 0 && get(0x10a00) == 0x12345678 && get(0x10bfc) == 0x12345678, "raw sector bytes not retained");
  for (uint32_t ordinal : {219u, 236u}) {
    check(transfer(ordinal, raw, 511) == 0xc000000d, "unaligned raw length accepted");
    check(transfer(ordinal, raw, 1024) == 0xc000000d, "oversized raw transfer accepted");
    check(transfer(ordinal, raw, 512, 0x10a00, 0) == 0xc00000bb, "implicit raw position accepted");
    check(transfer(ordinal, raw, 512, 0x10a00, 0x10700, 1) == 0xc00000bb, "async raw transfer accepted");
    put(0x10700, 0xa00);
    check(transfer(ordinal) == 0xc000000d, "out of bounds raw offset accepted");
    put(0x10700, 0x800); put(0x10704, 0xffffffff);
    check(transfer(ordinal) == 0xc000000d, "negative raw offset accepted");
    put(0x10704, 0);
    for (uint32_t n = 0; n < 4; ++n) {
      bool refused = false;
      try { transfer(ordinal, raw, 512, n == 0 ? 0x20000 : n == 1 ? 0x10840 : 0x10a00, n == 2 ? 0x20000 : 0x10700, 0, n == 3 ? 0x10a04 : 0x10840); }
      catch (const browser::Fault&) { refused = true; }
      check(refused && get(0x10840) == 0xabcdef, "invalid raw transfer partially published");
    }
  }
  for (uint32_t n = 0; n < 3; ++n) {
    bool refused = false;
    const auto owner = ctx.host;
    const auto kpcr = ctx.fs_base;
    browser::Runtime other({}, {});
    try {
      if (n == 0) { ctx.host = &other; transfer(236); }
      if (n == 1) { ctx.fs_base ^= 4; transfer(236); }
      if (n == 2) { auto lease = memory.Borrow(); transfer(236); }
    } catch (const browser::Fault&) { refused = true; }
    ctx.host = owner;
    ctx.fs_base = kpcr;
    check(refused, "raw transfer accepted foreign ownership or inherited lease");
  }
  check(transfer(219) == 0 && get(0x10a00) == 0x12345678, "failed raw transfer mutated storage");
  check(close(raw) && !close(raw), "raw cache handle lifetime broken");
  check(transfer(219) == 0xc0000008 && transfer(236) == 0xc0000008, "closed raw handle remained usable");
  check(open("D:\\default.xbe") == 0 && close(get(0x10100)), "session disc alias missing");
  const auto openLink = [&](std::string name, uint32_t output = 0x10100, uint32_t rootHandle = 0, uint32_t flags = 0x40) {
    memory.Write(ctx, 0x10300, name.data(), name.size());
    put(0x10200, uint32_t(name.size()) | (uint32_t(name.size()) << 16)); put(0x10204, 0x10300);
    put(0x10210, rootHandle); put(0x10214, 0x10200); put(0x10218, flags);
    put(0x10100, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(203), {output, 0x10210});
    check(ctx.esp == stack, "symbolic link open frame cleanup corrupted");
    if (result) { check(get(0x10100) == 0xabcdef, "failed symbolic link open published handle"); }
    return result;
  };
  check(openLink("\\??\\D:") == 0, "observed owned D alias open refused");
  const auto link = get(0x10100);
  check(open("default.xbe", link) == 0xc0000008, "symbolic link handle used as file root");
  check(openLink("\\??\\Q:") == 0xc0000034, "unknown link produced fake handle");
  check(openLink("\\??\\D:", 0x10100, link) == 0xc00000bb, "relative link object open admitted");
  check(openLink("\\??\\D:", 0x10100, 0, 0x400) == 0xc00000bb, "unsupported link object flags admitted");
  bool linkOutputRefused = false;
  try { openLink("\\??\\D:", 0x20000); }
  catch (const browser::Fault&) { linkOutputRefused = true; }
  check(linkOutputRefused && get(0x10100) == 0xabcdef, "invalid link output partially published");
  const auto queryLink = [&](uint32_t handle, uint32_t capacity = 520, uint32_t output = 0x30000, uint32_t descriptor = 0x10700, uint32_t required = 0x10720) {
    put(0x10700, 0xabcd | (capacity << 16)); put(0x10704, output); put(0x10720, 0xabcdef);
    std::array<uint8_t, 520> sentinel;
    sentinel.fill(0xa5);
    memory.Write(ctx, 0x30000, sentinel.data(), sentinel.size());
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(215), {handle, descriptor, required});
    check(ctx.esp == stack, "symbolic link query cleanup corrupted");
    return result;
  };
  check(queryLink(link) == 0 && get(0x10700) == (520u << 16 | 14) && get(0x10720) == 14, "owned link target counted result incorrect");
  std::array<char, 14> target;
  memory.Read(ctx, 0x30000, target.data(), target.size());
  check(std::string(target.data(), target.size()) == "\\DEVICE\\CDROM0" && (get(0x3000c) >> 16) == 0xa5a5, "symbolic link query invented target or appended terminator");
  check(queryLink(link, 13) == 0xc0000023 && get(0x10700) == (13u << 16 | 0xabcd) && get(0x10720) == 14 && get(0x30000) == 0xa5a5a5a5, "short link query partially changed target");
  check(queryLink(link, 14, 0x30000, 0x10700, 0) == 0 && get(0x10720) == 0xabcdef, "optional link required length output not optional");
  for (uint32_t n = 0; n < 5; ++n) {
    bool refused = false;
    try { queryLink(link, 520, n == 0 ? 0x20000 : n == 1 ? 0x10704 : 0x30000,
      n == 2 ? 0x20000 : 0x10700, n == 3 ? 0x10704 : n == 4 ? 0x20000 : 0x10720); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10700) == (520u << 16 | 0xabcd) && get(0x10720) == 0xabcdef && get(0x30000) == 0xa5a5a5a5, "invalid link query partially published output");
  }
  check(close(link) && !close(link), "symbolic link handle lifecycle broken");
  check(queryLink(link) == 0xc0000008 && get(0x10720) == 0xabcdef && get(0x30000) == 0xa5a5a5a5, "closed link target remained live");
  check(open("D:\\default.xbe") == 0, "shared handle quota fixture failed");
  const auto quotaFile = get(0x10100);
  std::array<uint32_t, 63> quotaLinks{};
  for (uint32_t n = 0; n < quotaLinks.size(); ++n) {
    check(openLink("\\??\\D:") == 0, "bounded link quota admitted too few handles");
    quotaLinks[n] = get(0x10100);
  }
  check(openLink("\\??\\D:") == 0xc000009a, "file and link handles bypassed shared bounded quota");
  for (const auto handle : quotaLinks) { check(close(handle), "bounded link handle did not retire"); }
  check(close(quotaFile), "shared handle quota file did not retire");
  check(createAlias("\\??\\D:", "\\Device\\CdRom0") == 0xc0000035, "observed existing disc alias did not collide truthfully");
  check(createAlias("\\??\\D:", "\\Device\\CdRom0") == 0xc0000035, "duplicate alias did not collide");
  check(createAlias("\\??\\X:", "\\Device\\Harddisk0\\Partition2") == 0xc00000bb, "alias mounted unknown device");
  check(createAlias("\\??\\X:", "\\Device\\Harddisk0\\Partition1\\..\\X") == 0xc0000033, "alias target traversal accepted");
  check(createAlias("\\??\\T:", "\\Device\\Harddisk0\\Partition1\\CACHE") == 0, "owned temporary alias failed");
  check(open("T:\\save.bin") == 0 && close(get(0x10100)), "temporary alias not bound to backed namespace");
  check(createAlias("\\??\\Z:", "\\??\\D:") == 0xc00000bb, "unbounded alias chain accepted");
  check(open("\\Device\\CdRom0", 0, 0x100001, 0x21) == 0, "selected disc root did not open");
  const auto root = get(0x10100);
  check(open("default.xbe", root) == 0, "root relative file did not open");
  const auto file = get(0x10100);
  check(open("D:\\default.xbe", 0, 0x100001, 0x20, 0x10100, 0x10108, 0) == 0xc0000043, "exclusive open ignored existing reader");
  check(root != file && close(file) && !close(file), "file handles alias or survive closure");
  check(open("\\??\\d:\\DEFAULT.XBE") == 0, "disc alias or case folding failed");
  check(close(get(0x10100)), "disc alias handle did not close");
  check(open("D:\\default.xbe", 0, 0x100001, 0x20, 0x10100, 0x10108, 0) == 0, "exclusive read open failed without readers");
  const auto exclusive = get(0x10100);
  check(open("D:\\default.xbe") == 0xc0000043, "new reader ignored exclusive handle");
  check(close(exclusive), "exclusive handle did not close");
  check(open("D:\\default.xbe", 0, 0x80000000) == 0, "generic read did not map synchronize access");
  check(close(get(0x10100)), "generic read handle did not close");
  check(open("D:\\absent") == 0xc0000034, "missing file did not return name not found");
  check(open("D:\\absent\\child") == 0xc000003a, "missing parent did not return path not found");
  check(open("D:\\default.xbe", 0, 2) == 0xc0000022, "write access accepted on read only disc");
  check(open("\\Device\\Harddisk0\\Partition1\\", 0, 0x100001, 0x800021) == 0, "observed temporary root open refused");
  const auto temporaryRoot = get(0x10100);
  const auto query = [&](uint32_t handle, uint32_t length = 24, uint32_t information = 3, uint32_t output = 0x10400, uint32_t status = 0x10420) {
    std::array<uint32_t, 6> sentinel;
    sentinel.fill(0xabcdef);
    memory.Write(ctx, 0x10400, sentinel.data(), sizeof(sentinel));
    put(0x10420, 0xabcdef); put(0x10424, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(218), {handle, status, output, length, information});
    check(ctx.esp == stack && get(status) == result && get(status + 4) == (result ? 0 : 24), "volume query status or stdcall cleanup corrupted");
    if (result) { check(get(0x10400) == 0xabcdef, "failed query changed output"); }
    return result;
  };
  check(query(temporaryRoot) == 0, "temporary volume query failed");
  check(get(0x10400) == 1024 && get(0x10404) == 0 && get(0x10408) == 1021 && get(0x1040c) == 0, "volume capacity does not reflect live allocations");
  check(get(0x10410) == 32 && get(0x10414) == 512, "virtual volume allocation units wrong");
  check(query(temporaryRoot, 23) == 0xc0000004, "short volume output accepted");
  check(query(temporaryRoot, 24, 7) == 0xc00000bb, "unimplemented volume class fabricated");
  check(query(0x70000004) == 0xc0000008, "thread handle accepted as volume");
  check(query(root) == 0xc00000bb, "disc geometry fabricated");
  const auto createDirectory = [&](std::string path, uint32_t disposition = 3, uint32_t rootHandle = 0, uint32_t output = 0x10100) {
    memory.Write(ctx, 0x10300, path.data(), path.size());
    put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(0x10204, 0x10300);
    put(0x10210, rootHandle); put(0x10214, 0x10200); put(0x10218, 0x40);
    put(0x10100, 0xabcdef); put(0x10108, 0xabcdef); put(0x1010c, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(190), {output, 0x100001, 0x10210, 0x10108, 0, 0x80, 3, disposition, 0x4021});
    check(ctx.esp == stack && get(0x10108) == result, "create directory status or cleanup corrupted");
    if (result) { check(get(0x10100) == 0xabcdef && get(0x1010c) == 0, "failed create published handle or information"); }
    return result;
  };
  check(createDirectory("\\Device\\Harddisk0\\Partition1\\TDATA") == 0 && get(0x1010c) == 2, "temporary directory was not created");
  check(close(get(0x10100)), "created directory did not close");
  check(createDirectory("TDATA", 3, temporaryRoot) == 0 && get(0x1010c) == 1, "existing temporary directory did not open");
  check(close(get(0x10100)), "opened directory did not close");
  check(createDirectory("TDATA", 2, temporaryRoot) == 0xc0000035, "exclusive directory create ignored collision");
  check(createDirectory("MISSING\\CHILD", 3, temporaryRoot) == 0xc000003a, "directory create synthesized missing parents");
  check(createDirectory("D:\\FORBIDDEN") == 0xc0000022, "directory create modified selected ISO");
  check(createDirectory("..\\ESCAPE", 3, temporaryRoot) == 0xc0000033, "directory create traversal accepted");
  check(query(temporaryRoot) == 0 && get(0x10408) == 1020, "guest directory creation not reflected in quota");
  const auto createFile = [&](std::string path, uint32_t disposition = 3, uint32_t output = 0x10100, uint32_t allocation = 0, uint32_t attributes = 4) {
    memory.Write(ctx, 0x10300, path.data(), path.size());
    put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(0x10204, 0x10300);
    put(0x10210, 0); put(0x10214, 0x10200); put(0x10218, 0x40);
    put(0x10100, 0xabcdef); put(0x10108, 0xabcdef); put(0x1010c, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(190), {output, 0x40100000, 0x10210, 0x10108, allocation, attributes, 1, disposition, 0x22});
    check(ctx.esp == stack && get(0x10108) == result, "create file status or cleanup corrupted");
    if (result) { check(get(0x10100) == 0xabcdef && get(0x1010c) == 0, "failed file create published partial outputs"); }
    return result;
  };
  check(createDirectory("\\Device\\Harddisk0\\Partition1\\UDATA") == 0 && close(get(0x10100)), "UDATA fixture creation failed");
  check(createDirectory("UDATA\\1234ABCD", 3, temporaryRoot) == 0 && close(get(0x10100)), "title fixture creation failed");
  const std::string titleFile = "\\Device\\Harddisk0\\partition1\\UDATA\\1234ABCD\\TitleMeta.xbx";
  check(createFile(titleFile) == 0 && get(0x1010c) == 2, "observed title metadata file OPEN_IF refused");
  const auto metadataFile = get(0x10100);
  const auto queryFile = [&](uint32_t handle, uint32_t length = 56, uint32_t information = 34, uint32_t output = 0x10800, uint32_t status = 0x10840) {
    std::array<uint32_t, 14> sentinel;
    sentinel.fill(0xabcdef);
    memory.Write(ctx, 0x10800, sentinel.data(), sizeof(sentinel));
    put(0x10840, 0xabcdef); put(0x10844, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(211), {handle, status, output, length, information});
    check(ctx.esp == stack && get(status) == result && get(status + 4) == (result ? 0 : 56), "file query status or cleanup corrupted");
    if (result) { check(get(0x10800) == 0xabcdef, "failed file query changed output"); }
    return result;
  };
  check(queryFile(metadataFile) == 0, "observed network-open metadata query refused");
  check(get(0x10820) == 0 && get(0x10824) == 0 && get(0x10828) == 0 && get(0x1082c) == 0 && get(0x10830) == 4 && get(0x10834) == 0, "new file EOF allocation attributes or padding fabricated");
  check(get(0x10804) != 0 && get(0x10804) == get(0x1080c) && get(0x10804) == get(0x10814) && get(0x10804) == get(0x1081c), "file creation timestamps missing");
  check(queryFile(metadataFile, 55) == 0xc0000004, "short file metadata buffer accepted");
  check(queryFile(metadataFile, 56, 5) == 0xc00000bb, "unobserved metadata class fabricated");
  check(queryFile(root) == 0 && get(0x10828) == 0 && get(0x10830) == 0x11 && get(0x10800) == 0 && get(0x10804) == 0,
    "ISO directory metadata lacks read-only attributes or invented original timestamps");
  check(queryFile(0x70000004) == 0xc0000008, "thread handle accepted as file metadata");
  for (uint32_t n = 0; n < 3; ++n) {
    bool refused = false;
    try { queryFile(metadataFile, 56, 34, n == 0 ? 0x20000 : n == 1 ? 0x10840 : 0x10800, n == 2 ? 0x10804 : 0x10840); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10800) == 0xabcdef && get(0x10840) == 0xabcdef, "invalid file metadata output partially published");
  }
  const auto writeFile = [&](uint32_t handle, uint32_t buffer = 0x10900, uint32_t length = 3, uint32_t event = 0, uint32_t offset = 0, uint32_t status = 0x10840) {
    put(0x10840, 0xabcdef); put(0x10844, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(236), {handle, event, 0, 0, status, buffer, length, offset});
    check(ctx.esp == stack && get(status) == result && get(status + 4) == (result ? 0 : length), "write status length or cleanup corrupted");
    return result;
  };
  const std::array<uint8_t, 3> payload{4, 8, 12};
  memory.Write(ctx, 0x10900, payload.data(), payload.size());
  check(writeFile(metadataFile) == 0 && writeFile(metadataFile, 0x10900, 2) == 0, "observed synchronous current-position write refused");
  check(queryFile(metadataFile) == 0 && get(0x10820) == 16384 && get(0x10828) == 5 && get(0x10830) == 4, "write did not update real EOF allocation and attributes");
  check(writeFile(metadataFile, 0x10900, 0) == 0, "zero-length write failed");
  check(writeFile(metadataFile, 0x10900, 3, 1) == 0xc00000bb, "unobserved event completion accepted");
  check(writeFile(metadataFile, 0x10900, 3, 0, 0x10700) == 0xc00000bb, "unobserved explicit offset accepted");
  check(writeFile(root) == 0xc0000022, "selected ISO accepted writes");
  check(writeFile(temporaryRoot) == 0xc00000ba, "directory accepted byte write");
  check(writeFile(0x70000004) == 0xc0000008, "thread handle accepted byte write");
  for (uint32_t n = 0; n < 3; ++n) {
    bool refused = false;
    try { writeFile(metadataFile, n == 0 ? 0x20000 : n == 1 ? 0x10840 : 0x10900, 3, 0, 0, n == 2 ? 0x20000 : 0x10840); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10840) == 0xabcdef, "invalid write partially published status");
    check(queryFile(metadataFile) == 0 && get(0x10828) == 5, "invalid write changed EOF or position");
  }
  check(createFile(titleFile) == 0xc0000043, "new writer ignored existing share-read handle");
  check(open(titleFile, 0, 0x100001, 0x20, 0x10100, 0x10108, 1) == 0xc0000043, "reader did not share existing writer");
  check(close(metadataFile) && !close(metadataFile), "metadata file handle lifetime broken");
  check(queryFile(metadataFile) == 0xc0000008, "closed metadata handle remained valid");
  check(createFile(titleFile) == 0 && get(0x1010c) == 1 && close(get(0x10100)), "existing metadata file was not opened");
  check(createFile(titleFile, 2) == 0xc0000035, "file CREATE ignored collision");
  check(createFile("D:\\NEW.XBX") == 0xc0000022, "file create modified selected ISO");
  check(createFile("\\Device\\Harddisk0\\partition1\\MISSING\\NEW.XBX") == 0xc000003a, "file create synthesized missing parent");
  put(0x10700, 1); put(0x10704, 0);
  check(createFile(titleFile, 3, 0x10100, 0x10700) == 0xc00000bb, "unobserved allocation request accepted");
  check(createFile(titleFile, 5) == 0xc00000bb, "unobserved overwrite disposition accepted");
  check(createFile(titleFile, 3, 0x10100, 0, 0x10) == 0xc000000d, "byte-file create accepted directory attribute");
  bool invalidFileRefused = false;
  try { createFile("\\Device\\Harddisk0\\partition1\\UDATA\\ATOMIC.XBX", 3, 0x20000); }
  catch (const browser::Fault&) { invalidFileRefused = true; }
  check(invalidFileRefused && open("UDATA\\ATOMIC.XBX", temporaryRoot) == 0xc0000034, "failed file publication left backing mutation");
  bool invalidCreateRefused = false;
  try { createDirectory("ATOMIC", 3, temporaryRoot, 0x20000); }
  catch (const browser::Fault&) { invalidCreateRefused = true; }
  check(invalidCreateRefused && open("ATOMIC", temporaryRoot) == 0xc0000034, "failed directory publication left backing mutation");
  for (uint32_t n = 0; n < 4; ++n) {
    bool refused = false;
    try { query(temporaryRoot, 24, 3, n == 0 ? 0x20000 : n == 1 ? 0x10420 : 0x10400, n == 2 ? 0x20000 : n == 3 ? 0x10404 : 0x10420); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10400) == 0xabcdef && get(0x10420) == 0xabcdef, "invalid volume output mutated state");
  }
  check(open("missing", temporaryRoot) == 0xc0000034, "temporary root is not a real directory");
  check(open("cache\\save.bin", temporaryRoot) == 0, "backed temporary file did not open relative to root");
  check(close(get(0x10100)), "temporary file did not close");
  check(open("\\Device\\Harddisk0\\Partition1\\CACHE", 0, 0x100001, 0x21) == 0, "backed temporary directory did not open");
  check(close(get(0x10100)), "temporary directory did not close");
  check(open("\\Device\\Harddisk0\\Partition1", 0, 0x100002, 0x21) == 0, "temporary root not writable");
  check(close(get(0x10100)), "temporary write handle did not close");
  check(open("D:\\default.xbe", temporaryRoot) == 0xc0000033, "temporary relative open escaped to disc");
  check(open("..\\default.xbe", temporaryRoot) == 0xc0000033, "temporary relative traversal accepted");
  check(open("\\Device\\Harddisk0\\Partition2\\", 0, 0x100001, 0x800021) == 0xc00000bb, "unmounted partition accepted");
  check(open("\\Device\\Harddisk0\\Partition3", 0, 0x100003, 0x18, 0x10100, 0x10108, 0) == 0, "cache raw format reopen failed");
  formatter = get(0x10100);
  const auto dismount = [&](uint32_t code = 0x90020, uint32_t output = 0, uint32_t length = 0, uint32_t status = 0x10840) {
    put(0x10840, 0xabcdef); put(0x10844, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(200), {formatter, 0, 0, 0, status, code, 0, 0, output, length});
    check(ctx.esp == stack && get(status) == result && get(status + 4) == 0, "cache dismount status or cleanup corrupted");
    return result;
  };
  check(dismount() == 0xc000014f, "invalid guest format dismounted successfully");
  rawBytes.fill(0xff);
  memory.Write(ctx, 0x30000, rawBytes.data(), rawBytes.size());
  put(0x30000, 0x58544146); put(0x30004, 0x12345678); put(0x30008, 32); put(0x3000c, 1); put(0x30010, 0xffff0000);
  check(rawPartitionTransfer(236, 0) == 0, "guest FATX header write failed");
  rawBytes.fill(0);
  memory.Write(ctx, 0x30000, rawBytes.data(), rawBytes.size());
  put(0x30000, 0xfffffff8);
  check(rawPartitionTransfer(236, 4096) == 0, "guest FATX allocation table write failed");
  rawBytes.fill(0xff);
  memory.Write(ctx, 0x30000, rawBytes.data(), rawBytes.size());
  for (uint32_t n = 0; n < 4; ++n) { check(rawPartitionTransfer(236, 8192 + 4096 * n) == 0, "guest FATX root cluster write failed"); }
  check(dismount(0x1234) == 0xc00000bb, "unknown filesystem control faked success");
  check(dismount(0x90020, 0x10a00, 4) == 0xc00000bb, "unobserved dismount output mode accepted");
  bool invalidDismountRefused = false;
  try { dismount(0x90020, 0, 0, 0x20000); }
  catch (const browser::Fault&) { invalidDismountRefused = true; }
  check(invalidDismountRefused && get(0x10840) == 0xabcdef, "invalid dismount published state");
  check(dismount() == 0, "real guest FATX bytes did not dismount");
  check(rawPartitionTransfer(236, 0) == 0xc00000a3, "dismounted raw handle retained write access");
  check(open("\\Device\\Harddisk0\\Partition3\\", 0, 0x100001, 0x800021) == 0xc00000a3, "namespace mounted before final raw close");
  check(close(formatter), "formatted cache raw handle did not close");
  check(open("\\Device\\Harddisk0\\Partition3\\", 0, 0x100001, 0x800021) == 0, "validated FATX namespace did not mount lazily");
  const auto cacheRoot = get(0x10100);
  check(query(cacheRoot) == 0 && get(0x10400) == 1023 && get(0x10408) == 1022 && get(0x10410) * get(0x10414) == 16384, "cache quota includes reserved FATX bytes or wrong allocation unit");
  check(open("\\Device\\Harddisk0\\Partition3", 0, 0x100003, 0x18, 0x10100, 0x10108, 0) == 0xc00000bb, "mounted namespace reopened stale raw disk");
  check(createDirectory("\\Device\\Harddisk0\\Partition3\\CACHE") == 0 && close(get(0x10100)), "cache namespace directory not backed");
  check(createFile("\\Device\\Harddisk0\\Partition3\\CACHE\\SAVE.BIN") == 0, "cache namespace byte file creation failed");
  const auto cacheFile = get(0x10100);
  check(writeFile(cacheFile) == 0 && writeFile(cacheFile, 0x10900, 2) == 0 && queryFile(cacheFile) == 0 && get(0x10828) == 5, "cache namespace retained no real bytes");
  check(close(cacheFile), "cache byte handle close failed");
  check(open("CACHE\\SAVE.BIN", temporaryRoot) == 0, "Partition1 same-path fixture missing");
  const auto partition1File = get(0x10100);
  check(queryFile(partition1File) == 0 && get(0x10828) == 3 && close(partition1File), "Partition3 writes changed Partition1 same path");
  check(open("CACHE\\SAVE.BIN", cacheRoot) == 0, "cache root relative file open routed incorrectly");
  const auto cacheReader = get(0x10100);
  check(queryFile(cacheReader) == 0 && get(0x10828) == 5 && close(cacheReader), "cache root relative open aliased Partition1");
  check(createAlias("\\??\\Z:", "\\Device\\Harddisk0\\Partition3\\") == 0, "cache mapped alias creation failed");
  check(open("Z:\\CACHE\\SAVE.BIN") == 0, "cache alias file open failed");
  const auto aliasReader = get(0x10100);
  check(queryFile(aliasReader) == 0 && get(0x10828) == 5 && close(aliasReader), "cache alias resolved to wrong backing");
  check(openExisting("z:\\default\\DynamicXbox.uxx") == 0xc000003a, "actual missing Z cache parent did not return PATH_NOT_FOUND");
  check(openExisting("Z:\\default.xbe", 0x64, 7) == 0xc0000034, "cache miss routed to existing disc bytes");
  check(openExisting("Z:\\CACHE\\ABSENT.BIN") == 0xc0000034, "cache missing leaf did not return NAME_NOT_FOUND");
  for (uint32_t options : {0x60u, 0x64u}) {
    check(openExisting("z:\\cache\\save.bin", options, 7) == 0, "existing cache OPEN_EXISTING failed");
    const auto existingCache = get(0x10100);
    check(queryFile(existingCache) == 0 && get(0x10828) == 5 && close(existingCache), "cache OPEN_EXISTING lost real backing");
    check(openExisting("CACHE\\SAVE.BIN", options, 7, cacheRoot) == 0 && close(get(0x10100)), "ordinary directory-relative OPEN_EXISTING failed");
  }
  check(openExisting("D:\\default.xbe", 0x60, 1, cacheRoot) == 0xc0000033, "ordinary cache root escaped to disc");
  check(openExisting("Z:\\default.xbe", 0x60, 1, 0xfffffffdu, 0x80100080, 1, 0) == 0xc0000034,
    "observed zero file attributes blocked truthful cache miss");
  put(0x10700, 1); put(0x10704, 0);
  check(openExisting("Z:\\CACHE\\SAVE.BIN", 0x60, 1, 0xfffffffdu, 0x80100080, 1, 0x80, 0x10700) == 0xc00000bb,
    "OPEN_EXISTING admitted allocation request");
  check(openExisting("Z:\\CACHE\\SAVE.BIN", 0x60, 1, 0xfffffffdu, 0xc0100080) == 0xc00000bb,
    "temporary backing broadened new mode to write access");
  check(query(cacheRoot) == 0 && get(0x10408) == 1020, "cache namespace accounting does not reflect real cluster use");
  const auto overwrite = [&](std::string path, uint32_t rootHandle = 0xfffffffdu, uint32_t output = 0x10100, uint32_t status = 0x10108,
      uint32_t access = 0x40100080, uint32_t options = 0x64, uint32_t allocation = 0) {
    return openExisting(path, options, 0, rootHandle, access, 5, 0, allocation, output, status);
  };
  check(overwrite("z:\\default\\DynamicXbox.umd") == 0xc000003a, "observed overwrite synthesized missing cache parent");
  check(overwrite("D:\\default.xbe") == 0xc0000022, "observed overwrite modified selected disc");
  check(createDirectory("DEFAULT", 3, cacheRoot) == 0 && close(get(0x10100)), "real cache destination parent creation failed");
  check(overwrite("z:\\Default\\DynamicXbox.umd") == 0 && get(0x1010c) == 2, "actual write-only FILE_OVERWRITE_IF frame refused creation");
  const auto destination = get(0x10100);
  check(queryFile(destination) == 0 && get(0x10828) == 0 && get(0x10830) == 0x80, "new overwrite destination metadata fabricated");
  const auto creationLow = get(0x10800), creationHigh = get(0x10804);
  check(runtime.Call(ctx, base, runtime.KernelTarget(219), {destination, 0, 0, 0, 0x10840, 0x10900, 3, 0}) == 0xc0000022,
    "write-only cache destination acquired read access");
  memory.Write(ctx, 0x10900, payload.data(), payload.size());
  check(writeFile(destination) == 0 && writeFile(destination, 0x10900, 2) == 0 && writeFile(destination, 0, 0) == 0,
    "actual write-only destination did not retain sequential writes");
  check(queryFile(destination) == 0 && get(0x10828) == 5 && get(0x10820) == 16384, "cache overwrite destination EOF or allocation wrong");
  check(query(cacheRoot) == 0 && get(0x10408) == 1018, "cache destination bytes not charged to real volume");
  check(overwrite("Z:\\DEFAULT\\DYNAMICXBOX.UMD") == 0xc0000043 && queryFile(destination) == 0 && get(0x10828) == 5,
    "exclusive overwrite collision truncated live destination");
  check(openExisting("Z:\\DEFAULT\\DYNAMICXBOX.UMD", 0x64, 7) == 0xc0000043, "actual exclusive destination admitted reader");
  check(writeFile(destination, 0x10900, 3, 1) == 0xc00000bb && writeFile(destination, 0x10900, 3, 0, 0x10700) == 0xc00000bb,
    "new overwrite mode broadened event or explicit-offset writes");
  for (uint32_t n = 0; n < 3; ++n) {
    bool refused = false;
    try { writeFile(destination, n == 0 ? 0x20000 : n == 1 ? 0x10840 : 0x10900, 3, 0, 0, n == 2 ? 0x20000 : 0x10840); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10840) == 0xabcdef && queryFile(destination) == 0 && get(0x10828) == 5,
      "bad actual destination write changed payload or outputs");
  }
  check(writeFile(destination, 0x10900, 1) == 0 && queryFile(destination) == 0 && get(0x10828) == 6 && close(destination),
    "failed write advanced actual destination cursor");
  for (uint32_t n = 0; n < 3; ++n) {
    bool refused = false;
    try { overwrite("Z:\\DEFAULT\\DYNAMICXBOX.UMD", 0xfffffffdu, n == 0 ? 0x20000 : n == 1 ? 0x10108 : 0x10100, n == 2 ? 0x20000 : 0x10108); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10100) == 0xabcdef && get(0x10108) == 0xabcdef, "bad overwrite outputs published partial handle");
  }
  check(openExisting("Z:\\DEFAULT\\DYNAMICXBOX.UMD", 0x64, 7) == 0, "existing cache payload vanished after bad overwrite");
  const auto retainedDestination = get(0x10100);
  check(queryFile(retainedDestination) == 0 && get(0x10828) == 6, "bad overwrite outputs truncated destination");
  check(overwrite("Z:\\DEFAULT\\DYNAMICXBOX.UMD") == 0xc0000043 && close(retainedDestination), "overwrite bypassed existing reader exclusion");
  check(overwrite("DEFAULT\\DYNAMICXBOX.UMD", cacheRoot) == 0 && get(0x1010c) == 3, "existing destination did not report FILE_OVERWRITTEN");
  const auto truncated = get(0x10100);
  check(queryFile(truncated) == 0 && get(0x10828) == 0 && get(0x10820) == 0 && get(0x10800) == creationLow && get(0x10804) == creationHigh,
    "overwrite lost creation timestamp or did not truncate backing");
  check(query(cacheRoot) == 0 && get(0x10408) == 1019, "overwrite did not release real allocation");
  check(writeFile(truncated, 0x10900, 1) == 0 && queryFile(truncated) == 0 && get(0x10828) == 1, "overwrite did not reset new cursor");
  const auto seek = [&](uint32_t handle, uint64_t position, uint32_t length = 8, uint32_t information = 14, uint32_t input = 0x10700, uint32_t status = 0x10840) {
    put(0x10700, uint32_t(position)); put(0x10704, uint32_t(position >> 32));
    put(0x10840, 0xabcdef); put(0x10844, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(226), {handle, status, input, length, information});
    check(ctx.esp == stack && get(status) == result && get(status + 4) == 0, "actual226 position status, information or stdcall corrupt");
    return result;
  };
  check(seek(truncated, 4) == 0 && queryFile(truncated) == 0 && get(0x10828) == 1 && writeFile(truncated, 0x10900, 1) == 0 &&
    queryFile(truncated) == 0 && get(0x10828) == 5, "actual cache position did not move cursor independently of EOF");
  check(seek(truncated, UINT64_MAX) == 0xc000000d && seek(truncated, 1, 7) == 0xc0000004 && seek(truncated, 1, 8, 20) == 0xc00000bb,
    "cache position admitted negative, short or unrelated set operation");
  for (uint32_t n = 0; n < 3; ++n) {
    bool refused = false;
    try { seek(truncated, 1, 8, 14, n == 0 ? 0x20000 : n == 1 ? 0x10840 : 0x10700, n == 2 ? 0x20000 : 0x10840); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10840) == 0xabcdef, "bad cache position span partially published outputs");
  }
  check(writeFile(truncated, 0x10900, 1) == 0 && queryFile(truncated) == 0 && get(0x10828) == 6 && close(truncated),
    "failed cache position changed write cursor");
  check(overwrite("\\Device\\Harddisk0\\Partition1\\CACHE\\OVERWRITE.BIN", 0) == 0 && get(0x1010c) == 2,
    "actual overwrite mode unavailable in owned temporary namespace");
  const auto temporaryDestination = get(0x10100);
  memory.Write(ctx, 0x10900, payload.data(), payload.size());
  check(writeFile(temporaryDestination) == 0 && writeFile(temporaryDestination, 0x10900, 2) == 0 && close(temporaryDestination),
    "temporary destination lost sequential bytes");
  check(overwrite("\\Device\\Harddisk0\\Partition1\\CACHE\\READONLY.BIN", 0) == 0xc0000022, "readonly backing accepted overwrite");
  check(open("\\Device\\Harddisk0\\Partition1\\CACHE\\READONLY.BIN", 0, 0x40100000) == 0xc0000022,
    "readonly backing accepted write handle");
  put(0x10700, 1); put(0x10704, 0);
  check(overwrite("Z:\\DEFAULT\\DYNAMICXBOX.UMD", 0xfffffffdu, 0x10100, 0x10108, 0x40100080, 0x64, 0x10700) == 0xc00000bb,
    "actual overwrite admitted unobserved allocation");
  for (uint32_t options : {0x60u, 0x44u, 0x74u}) {
    check(overwrite("Z:\\DEFAULT\\DYNAMICXBOX.UMD", 0xfffffffdu, 0x10100, 0x10108, 0x40100080, options) == 0xc00000bb,
      "actual overwrite broadened unsupported synchronous mode");
  }
  check(overwrite("Z:\\DEFAULT\\READBACK.UMD") == 0 && get(0x1010c) == 2, "retained cache header writer did not open");
  const auto headerWriter = get(0x10100);
  const std::array<uint8_t, 20> header{0x55, 0x4d, 0x44, 0x30, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  memory.Write(ctx, 0x10900, header.data(), header.size());
  check(writeFile(headerWriter, 0x10900, 7) == 0 && writeFile(headerWriter, 0x10907, 13) == 0 && close(headerWriter),
    "real cache header writes failed");
  check(openExisting("Z:\\DEFAULT\\READBACK.UMD", 0x60, 1, 0xfffffffdu, 0x80100080) == 0,
    "actual cache read190 mode refused retained backing");
  const auto headerReader = get(0x10100);
  check(overwrite("Z:\\DEFAULT\\READBACK.UMD") == 0xc0000043, "real cache reader allowed destructive overwrite");
  const auto readFile = [&](uint32_t handle, uint32_t length = 20, uint32_t buffer = 0x10a00, uint32_t event = 0, uint32_t offset = 0,
      uint32_t status = 0x10840, uint32_t apc = 0, uint32_t context = 0) {
    put(0x10840, 0xabcdef); put(0x10844, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(219), {handle, event, apc, context, status, buffer, length, offset});
    check(ctx.esp == stack && get(status) == result && (result ? get(status + 4) == 0 : get(status + 4) <= length),
      "cache read219 status, count or stdcall corrupt");
    return result;
  };
  std::array<uint8_t, 20> delivered{};
  check(queryFile(headerReader) == 0 && readFile(headerReader) == 0 && get(0x10844) == header.size(),
    "observed actual cache219 null-offset header read refused");
  memory.Read(ctx, 0x10a00, delivered.data(), delivered.size());
  check(delivered == header, "cache219 fabricated bytes instead of retained236 backing");
  check(readFile(headerReader, 1) == 0xc0000011 && readFile(headerReader, 0, 0) == 0 && get(0x10844) == 0,
    "cache219 EOF or zero-byte completion corrupt");
  check(seek(headerReader, 10) == 0 && readFile(headerReader) == 0 && get(0x10844) == 10,
    "cache seek/read did not return partial retained tail");
  memory.Read(ctx, 0x10a00, delivered.data(), 10);
  check(std::equal(delivered.begin(), delivered.begin() + 10, header.begin() + 10) && readFile(headerReader, 1) == 0xc0000011,
    "cache partial EOF bytes or cursor corrupt");
  check(seek(headerReader, 0x100000000ull) == 0 && readFile(headerReader, 1) == 0xc0000011 && seek(headerReader, 0) == 0,
    "cache large positive seek wrapped into backing bytes");
  for (uint32_t n = 0; n < 5; ++n) {
    bool refused = false;
    try { readFile(headerReader, 3, n == 0 ? 0x20000 : n == 1 ? 0x10840 : n == 2 ? ctx.esp - 4 : 0x10a00, 0, 0,
      n == 3 ? 0x20000 : n == 4 ? 0x10841 : 0x10840); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10840) == 0xabcdef, "bad cache219 span partially published outputs");
  }
  check(readFile(headerReader, 3, 0x10a00, 1) == 0xc00000bb && readFile(headerReader, 3, 0x10a00, 0, 0, 0x10840, 1) == 0xc00000bb &&
    readFile(headerReader, 3, 0x10a00, 0, 0, 0x10840, 0, 1) == 0xc00000bb &&
    readFile(headerReader, 3, 0x10a00, 0, 0x10700) == 0xc00000bb && readFile(headerReader, (1u << 20) + 1) == 0xc00000bb,
    "cache219 broadened unobserved async, explicit-offset or unbounded modes");
  check(readFile(headerReader, 3) == 0 && get(0x10844) == 3 && (get(0x10a00) & 0xffffff) == 0x444d55,
    "failed cache reads advanced handle cursor or lost first header bytes");
  check(writeFile(headerReader) == 0xc0000022 && readFile(0x70000004, 1) == 0xc0000008 && close(headerReader) &&
    readFile(headerReader, 1) == 0xc0000008, "cache read/write access or closed handle ownership lost");
  check(openExisting("\\Device\\Harddisk0\\Partition1\\CACHE\\READONLY.BIN", 0x60, 1, 0, 0x80100080) == 0,
    "readonly owned backing refused read-only cache mode");
  const auto readonlyReader = get(0x10100);
  check(readFile(readonlyReader, 1) == 0xc0000011 && writeFile(readonlyReader) == 0xc0000022 && close(readonlyReader),
    "readonly read either fabricated empty bytes or gained write access");
  check(close(cacheRoot), "cache root did not close after overwrite tests");
  check(close(temporaryRoot) && !close(temporaryRoot), "temporary root handle lifetime broken");
  check(open("missing", temporaryRoot) == 0xc0000008, "closed temporary root remained valid");
  check(query(temporaryRoot) == 0xc0000008, "closed volume handle remained valid");
  check(open("D:\\..\\default.xbe") == 0xc0000033, "path traversal accepted");
  check(open(std::string("D:\\default.xbe\0x", 16)) == 0xc0000033, "embedded nul accepted");
  check(open("D:\\", 0, 1, 0x40) == 0xc00000ba, "directory accepted as file");
  check(open("D:\\default.xbe", 0, 1, 1) == 0xc0000103, "file accepted as directory");
  check(open("D:\\default.xbe", 0, 1, 0x1000) == 0xc00000bb, "delete on close accepted");
  check(open("relative", 0x70000004) == 0xc0000008, "foreign handle used as root");
  check(close(root), "root did not close");
  check(open("default.xbe", root) == 0xc0000008, "closed root remained valid");
  for (uint32_t n = 0; n < 3; ++n) {
    bool refused = false;
    try { open("D:\\default.xbe", 0, 1, 0x20, n == 0 ? 0x20000 : n == 1 ? 0x10108 : 0x10100, n == 2 ? 0x20000 : 0x10108); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10100) == 0xabcdef && get(0x10108) == 0xabcdef, "invalid output mutated state");
  }
  const auto kpcr = ctx.fs_base;
  ctx.fs_base ^= 4;
  bool foreignContextRefused = false;
  try { open("\\Device\\Harddisk0\\Partition1\\", 0, 0x100001, 0x800021); }
  catch (const browser::Fault&) { foreignContextRefused = true; }
  ctx.fs_base = kpcr;
  check(foreignContextRefused, "forged worker context accessed temporary namespace");
  for (uint32_t n = 0; n < 64; ++n) { check(open("D:\\default.xbe") == 0, "bounded file table filled early"); }
  check(open("D:\\default.xbe") == 0xc000009a, "file handle capacity bypassed");
  check(overwrite("\\Device\\Harddisk0\\Partition1\\CACHE\\OVERWRITE.BIN", 0) == 0xc000009a &&
    overwrite("\\Device\\Harddisk0\\Partition1\\CACHE\\UNPUBLISHED.BIN", 0) == 0xc000009a,
    "actual overwrite bypassed shared handle quota");
  const auto stack = ctx.esp;
  for (uint32_t n = 0; n < 4; ++n) {
    bool refused = false;
    const auto owner = ctx.host;
    browser::Runtime other({}, {});
    try {
      if (n == 0) { ctx.host = &other; runtime.Kernel(ctx, base, 202); }
      if (n == 1) { browser::Memory foreign; runtime.Kernel(ctx, foreign.Base(), 202); }
      if (n == 2) { ctx.esp = 0xfffffff0; runtime.Kernel(ctx, base, 202); }
      if (n == 3) { auto lease = memory.Borrow(); runtime.Kernel(ctx, base, 202); }
    } catch (const browser::Fault&) { refused = true; }
    ctx.host = owner; ctx.esp = stack;
    check(refused, "foreign ownership, overflowing frame or backing lease accepted");
  }
  ctx.esp += 8;
};

const auto testTemporaryStorage = [] {
  browser::TemporaryFilesystem storage(4 * 16384, 8);
  const auto initial = storage.Usage();
  check(initial.capacity == 65536 && initial.allocated == 16384 && initial.entries == 1, "temporary root quota is not reserved");
  check(storage.Stat("")->directory && !storage.Stat("MISSING"), "temporary root metadata fabricated");
  check(storage.CreateDirectory("cache") == 0, "temporary directory creation failed");
  const std::array<uint8_t, 3> payload = {7, 8, 9};
  check(storage.WriteFile("cache\\save.bin", payload) == 0, "temporary write failed");
  std::array<uint8_t, 8> output{};
  check(storage.Read("CACHE\\SAVE.BIN", 1, output) == 2 && output[0] == 8 && output[1] == 9, "temporary read did not return stored bytes");
  check(storage.Stat("CACHE\\SAVE.BIN")->size == 3 && storage.Usage().allocated == 49152, "temporary allocation accounting wrong");
  check(storage.WriteFile("CACHE\\SAVE.BIN", std::vector<uint8_t>(32769, 4)) == 0xc000007f, "temporary capacity exceeded");
  check(storage.Stat("CACHE\\SAVE.BIN")->size == 3 && storage.Usage().allocated == 49152, "failed write changed bytes or quota");
  check(storage.WriteFile("CACHE\\SAVE.BIN", {}) == 0 && storage.Usage().allocated == 32768, "truncate did not release quota");
  check(storage.WriteFile("CACHE", payload) == 0xc00000ba, "directory overwritten as a file");
  check(storage.WriteFile("ABSENT\\FILE", payload) == 0xc000003a, "write created missing parents");
  check(storage.WriteFile("..\\FILE", payload) == 0xc0000033, "backing traversal escaped root");
  check(storage.CreateDirectory("cache") == 0xc0000035, "directory collision ignored");
  std::atomic<uint32_t> failures = 0;
  std::array<std::thread, 2> workers;
  for (uint32_t n = 0; n < workers.size(); ++n) {
    workers[n] = std::thread([&, n] {
      for (uint32_t count = 0; count < 64; ++count) {
        if (storage.WriteFile("CACHE\\WORK" + std::to_string(n), payload) != 0) { ++failures; }
      }
    });
  }
  for (auto& worker : workers) { worker.join(); }
  check(failures == 0 && storage.Usage().allocated == 65536, "concurrent writes bypassed capacity or lost files");
  check(storage.WriteFile("FULL", payload) == 0xc000007f, "full storage accepted write");
  browser::TemporaryFilesystem isolated(16384, 1);
  check(!isolated.Stat("CACHE") && isolated.Usage().entries == 1, "sessions share temporary content");
  check(isolated.WriteFile("EMPTY", {}) == 0xc000009a, "metadata bound ignored");
};

const auto testConcurrentProvider = [](std::shared_ptr<const kernel::DiscIndex> disc) {
  std::weak_ptr<browser::TemporaryFilesystem> lifetime;
  {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Map(0x10000, 0x10000); image.memory->Commit(0x10000, 0x10000);
    image.image.kernel_thunk = 0x10000;
    auto temporary = std::make_shared<browser::TemporaryFilesystem>();
    lifetime = temporary;
    auto close = browser::InstallFileProvider(runtime, image, std::move(disc), temporary);
    browser::InstallThreadProvider(runtime, image, close);
    browser::BindImageImports(image, runtime, {});
    std::atomic<uint32_t> ready = 0, created = 0;
    for (uint32_t n = 0; n < 2; ++n) {
      X86Context context{};
      context.host = &runtime; context.fs_base = 0x18000 + n * 4096; context.esp = 0x13000 + n * 0x4000;
      runtime.Launch(context, image.memory->Base(), [&, n](X86Context& ctx, uint8_t* base) {
        auto& memory = *image.memory;
        const uint32_t arena = 0x11000 + n * 0x4000;
        const auto put = [&](uint32_t offset, uint32_t value) { memory.Write(ctx, arena + offset, &value, 4); };
        const auto get = [&](uint32_t offset) { uint32_t value; memory.Read(ctx, arena + offset, &value, 4); return value; };
        const std::string path = "\\Device\\Harddisk0\\Partition1\\RACE";
        memory.Write(ctx, arena + 0x100, path.data(), path.size());
        put(0, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(4, arena + 0x100);
        put(8, 0); put(12, arena); put(16, 0x40);
        ++ready;
        while (ready != 2) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
        for (uint32_t count = 0; count < 32; ++count) {
          check(runtime.Call(ctx, base, runtime.KernelTarget(190), {arena + 32, 0x100001, arena + 8, arena + 40, 0, 0x80, 3, 3, 0x4021}) == 0, "concurrent OPEN_IF failed");
          if (get(44) == 2) { ++created; }
          else { check(get(44) == 1, "concurrent OPEN_IF information wrong"); }
          const auto handle = get(32);
          check(runtime.Call(ctx, base, runtime.KernelTarget(218), {handle, arena + 40, arena + 64, 24, 3}) == 0 && get(72) == 1022, "concurrent volume quota torn");
          check(runtime.Call(ctx, base, runtime.KernelTarget(187), {handle}) == 0, "concurrent handle did not close");
          check(runtime.Call(ctx, base, runtime.KernelTarget(187), {handle}) == 0xc0000008, "concurrent close left live handle");
        }
      });
    }
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(ready == 2 && created == 1 && runtime.ActiveWorkers() == 0, "concurrent create was not serialized exactly once");
    check(temporary->Usage().entries == 2, "concurrent directory duplicated metadata");
    std::atomic<bool> cancelled = false;
    X86Context context{}; context.host = &runtime; context.esp = 0x13000;
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      const std::array<uint32_t, 10> frame = {0, 0x11020, 0x100001, 0x11008, 0x11028, 0, 0x80, 3, 3, 0x4021};
      image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      const uint32_t sentinel = 0xabcdef;
      image.memory->Write(ctx, 0x11020, &sentinel, 4);
      image.memory->Write(ctx, 0x11028, &sentinel, 4);
      runtime.Cancel();
      try { runtime.Kernel(ctx, base, 190); }
      catch (const browser::Cancelled&) { cancelled = true; }
      uint32_t output = 0, status = 0;
      image.memory->Read(ctx, 0x11020, &output, 4); image.memory->Read(ctx, 0x11028, &status, 4);
      check(output == sentinel && status == sentinel, "cancelled create published outputs");
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(cancelled && runtime.ActiveWorkers() == 0 && temporary->Usage().entries == 2, "cancelled namespace operation retained workers or mutated backing");
    close = {}; temporary.reset();
    check(!lifetime.expired(), "runtime lost its temporary backing early");
  }
  check(lifetime.expired(), "temporary backing survived session teardown");
};

const auto testConcurrentOverwrite = [] {
  browser::Runtime runtime({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Map(0x10000, 0x10000); image.memory->Commit(0x10000, 0x10000);
  image.image.kernel_thunk = 0x10000;
  auto temporary = std::make_shared<browser::TemporaryFilesystem>(32768);
  auto close = browser::InstallFileProvider(runtime, image, {}, temporary);
  browser::BindImageImports(image, runtime, {});
  std::atomic<uint32_t> ready = 0, created = 0, overwritten = 0;
  for (uint32_t n = 0; n < 2; ++n) {
    X86Context context{};
    context.host = &runtime; context.fs_base = 0x18000 + n * 4096; context.esp = 0x13000 + n * 0x4000;
    runtime.Launch(context, image.memory->Base(), [&, n](X86Context& ctx, uint8_t* base) {
      auto& memory = *image.memory;
      const uint32_t arena = 0x11000 + n * 0x4000;
      const auto put = [&](uint32_t offset, uint32_t value) { memory.Write(ctx, arena + offset, &value, 4); };
      const auto get = [&](uint32_t offset) { uint32_t value; memory.Read(ctx, arena + offset, &value, 4); return value; };
      const std::string path = "\\Device\\Harddisk0\\Partition1\\DEST";
      memory.Write(ctx, arena + 0x100, path.data(), path.size());
      put(0, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(4, arena + 0x100);
      put(8, 0); put(12, arena); put(16, 0x40);
      put(64, n ? 0x222222 : 0x111111);
      ++ready;
      while (ready != 2) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
      for (uint32_t count = 0; count < 32; ++count) {
        check(runtime.Call(ctx, base, runtime.KernelTarget(190), {arena + 32, 0x40100080, arena + 8, arena + 40, 0, 0, 3, 5, 0x64}) == 0,
          "threaded observed overwrite failed");
        if (get(44) == 2) { ++created; }
        else { check(get(44) == 3, "threaded overwrite information fabricated"); ++overwritten; }
        const auto handle = get(32);
        put(72, 0); put(76, 0);
        check(runtime.Call(ctx, base, runtime.KernelTarget(226), {handle, arena + 40, arena + 72, 8, 14}) == 0 && get(44) == 0,
          "threaded actual destination position failed");
        check(runtime.Call(ctx, base, runtime.KernelTarget(236), {handle, 0, 0, 0, arena + 40, arena + 64, 3, 0}) == 0 && get(44) == 3,
          "threaded overwrite writer failed");
        check(close(handle), "threaded overwrite handle did not close");
      }
    });
  }
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  std::array<uint8_t, 3> bytes{};
  check(ready == 2 && created == 1 && overwritten == 63 && runtime.ActiveWorkers() == 0 && temporary->Usage().entries == 2 &&
    temporary->Usage().allocated == 32768 && temporary->Read("DEST", 0, bytes) == 3 &&
    (bytes == std::array<uint8_t, 3>{0x11, 0x11, 0x11} || bytes == std::array<uint8_t, 3>{0x22, 0x22, 0x22}),
    "threaded overwrites tore payload, duplicated metadata, lost quota or misreported create");
};

const auto testTemporaryMetadata = [] {
  uint64_t clock = 133000000000000000;
  browser::TemporaryFilesystem storage(2 * 16384, 3, [&] { return clock; });
  check(storage.Stat("")->creationTime == clock, "temporary root has no real creation metadata");
  check(storage.CreateFile("META.XBX", 4) == 0, "empty metadata file creation failed");
  const auto created = storage.Stat("META.XBX");
  check(created && created->size == 0 && created->attributes == 4 && created->creationTime == clock && created->lastAccessTime == clock && created->lastWriteTime == clock && created->changeTime == clock, "created metadata not backed by local operation");
  check(storage.Usage().allocated == 16384 && storage.Usage().entries == 2, "empty file reserves nonexistent payload");
  ++clock;
  check(storage.WriteFile("meta.xbx", std::array<uint8_t, 1>{7}) == 0, "backed file write failed");
  const auto written = storage.Stat("META.XBX");
  check(written->creationTime == created->creationTime && written->lastAccessTime == created->lastAccessTime && written->lastWriteTime == clock && written->changeTime == clock && written->attributes == 4, "write replaced creation or attribute metadata");
  ++clock;
  std::array<uint8_t, 1> byte{};
  check(storage.Read("META.XBX", 0, byte) == 1 && byte[0] == 7 && storage.Stat("META.XBX")->lastAccessTime == clock, "real file read did not update local access metadata");
  ++clock;
  check(storage.Write("META.XBX", 3, std::array<uint8_t, 1>{9}) == 0 && storage.Stat("META.XBX")->size == 4, "bounded offset write failed");
  std::array<uint8_t, 4> contents{};
  check(storage.Read("META.XBX", 0, contents) == 4 && contents == std::array<uint8_t, 4>{7, 0, 0, 9}, "offset write did not preserve data or zero real gap");
  const auto previous = storage.Stat("META.XBX");
  check(storage.Write("META.XBX", 32767, std::array<uint8_t, 2>{1, 2}) == 0xc000007f && storage.Stat("META.XBX")->size == 4 && storage.Stat("META.XBX")->lastWriteTime == previous->lastWriteTime, "quota failure partially mutated payload or metadata");
  check(storage.Write("META.XBX", UINT64_MAX, {}) == 0 && storage.Stat("META.XBX")->size == 4, "zero write extended EOF");
  check(storage.CreateFile("SECOND", 4) == 0 && storage.CreateFile("THIRD", 4) == 0xc000009a && storage.Usage().entries == 3, "empty file bypassed metadata quota");
};

const auto testOverwriteStorage = [] {
  uint64_t clock = 133000000000000000;
  browser::TemporaryFilesystem storage(32768, 4, [&] { return clock; });
  check(storage.CreateFile("DEST", 4) == 0 && storage.Write("DEST", 0, std::array<uint8_t, 3>{1, 2, 3}) == 0, "overwrite fixture creation failed");
  const auto original = storage.Stat("DEST");
  const auto entries = storage.Usage().entries;
  ++clock;
  check(storage.OverwriteFile("dest", 0x80) == 0, "backed overwrite failed");
  const auto truncated = storage.Stat("DEST");
  std::array<uint8_t, 3> bytes{9, 9, 9};
  check(truncated->size == 0 && truncated->attributes == 0x80 && truncated->creationTime == original->creationTime &&
    truncated->lastAccessTime == original->lastAccessTime && truncated->lastWriteTime == clock && truncated->changeTime == clock &&
    storage.Usage().allocated == 16384 && storage.Usage().entries == entries && storage.Read("DEST", 0, bytes) == 0 && bytes[0] == 9,
    "overwrite lost metadata, retained old bytes, or failed to release quota");
  check(storage.Write("DEST", 0, std::array<uint8_t, 1>{7}) == 0 && storage.Read("DEST", 0, bytes) == 1 && bytes[0] == 7,
    "truncated file could not retain replacement data");
  check(storage.OverwriteFile("MISSING", 0x80) == 0xc0000034 && storage.OverwriteFile("MISSING\\CHILD", 0x80) == 0xc000003a &&
    storage.OverwriteFile("..\\DEST", 0x80) == 0xc0000033 && storage.OverwriteFile("", 0x80) == 0xc00000ba,
    "overwrite fabricated missing names, parents or directories");
  check(storage.CreateFile("READONLY", 1) == 0, "readonly fixture failed");
  const auto readonly = storage.Stat("READONLY");
  ++clock;
  check(storage.OverwriteFile("READONLY", 0x80) == 0xc0000022 && storage.Write("READONLY", 0, std::array<uint8_t, 1>{7}) == 0xc0000022 &&
    storage.WriteFile("READONLY", std::array<uint8_t, 1>{7}) == 0xc0000022 && storage.Stat("READONLY")->attributes == 1 &&
    storage.Stat("READONLY")->lastWriteTime == readonly->lastWriteTime && storage.Usage().allocated == 32768,
    "readonly refusal mutated bytes, metadata or quota");
};

const auto testOverwriteCancellation = [] {
  for (uint32_t n = 0; n < 4; ++n) {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Map(0x10000, 4096); image.memory->Commit(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    auto temporary = std::make_shared<browser::TemporaryFilesystem>(32768);
    check(temporary->WriteFile("TARGET", std::array<uint8_t, 3>{1, 2, 3}) == 0, "overwrite cancellation fixture failed");
    auto close = browser::InstallFileProvider(runtime, image, {}, temporary);
    browser::BindImageImports(image, runtime, {});
    X86Context context{}; context.host = &runtime; context.fs_base = 0x10800; context.esp = 0x10f00;
    std::atomic<bool> cancelled = false;
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      auto& memory = *image.memory;
      const auto put = [&](uint32_t at, uint32_t value) { memory.Write(ctx, at, &value, 4); };
      const auto get = [&](uint32_t at) { uint32_t value; memory.Read(ctx, at, &value, 4); return value; };
      const std::string path = std::string("\\Device\\Harddisk0\\Partition1\\") + (n == 1 ? "MISSING" : "TARGET");
      memory.Write(ctx, 0x10300, path.data(), path.size());
      put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(0x10204, 0x10300);
      put(0x10210, 0); put(0x10214, 0x10200); put(0x10218, 0x40);
      std::array<uint32_t, 10> frame{0, 0x10100, 0x40100080, 0x10210, 0x10108, 0, 0, 0, 5, 0x64};
      if (n >= 2) {
        check(runtime.Call(ctx, base, runtime.KernelTarget(190), {0x10100, 0x40100080, 0x10210, 0x10108, 0, 0, 0, 5, 0x64}) == 0 && get(0x1010c) == 3,
          "actual overwrite cancellation write fixture refused");
        const auto handle = get(0x10100);
        put(0x10400, 0x76543210);
        check(runtime.Call(ctx, base, runtime.KernelTarget(236), {handle, 0, 0, 0, 0x10108, 0x10400, 4, 0}) == 0,
          "actual destination initial write failed");
        frame = n == 2 ? std::array<uint32_t, 10>{0, handle, 0, 0, 0, 0x10108, 0x10400, 4, 0, 0} :
          std::array<uint32_t, 10>{0, handle, 0x10108, 0x10400, 8, 14, 0, 0, 0, 0};
        put(0x10404, 0);
      }
      const auto metadata = temporary->Stat("TARGET");
      const auto usage = temporary->Usage();
      memory.Write(ctx, ctx.esp, frame.data(), (n == 2 ? 9 : n == 3 ? 6 : 10) * 4);
      put(0x10100, 0xabcdef); put(0x10108, 0xabcdef); put(0x1010c, 0xabcdef);
      runtime.Cancel();
      try { runtime.Kernel(ctx, base, n == 2 ? 236 : n == 3 ? 226 : 190); }
      catch (const browser::Cancelled&) { cancelled = true; }
      const auto retained = temporary->Stat("TARGET");
      check(get(0x10100) == 0xabcdef && get(0x10108) == 0xabcdef && get(0x1010c) == 0xabcdef && retained->size == metadata->size &&
        retained->attributes == metadata->attributes && retained->lastWriteTime == metadata->lastWriteTime && retained->changeTime == metadata->changeTime &&
        temporary->Usage().allocated == usage.allocated && temporary->Usage().entries == usage.entries && !temporary->Stat("MISSING"),
        "cancelled actual overwrite changed bytes, metadata, quota or outputs");
      std::array<uint8_t, 4> bytes{};
      check(temporary->Read("TARGET", 0, bytes) == (n >= 2 ? 4u : 3u) && (n >= 2 ? bytes == std::array<uint8_t, 4>{0x10, 0x32, 0x54, 0x76} : bytes == std::array<uint8_t, 4>{1, 2, 3, 0}),
        "cancelled overwrite or write changed retained payload");
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(cancelled && runtime.ActiveWorkers() == 0, "actual overwrite cancellation retained worker");
  }
};

const auto testReadCommitBoundary = [] {
  browser::Runtime runtime({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Map(0x10000, 4096); image.memory->Commit(0x10000, 4096);
  image.image.kernel_thunk = 0x10000;
  bool armed = false;
  uint64_t clock = 133000000000000000;
  auto temporary = std::make_shared<browser::TemporaryFilesystem>(32768, 4, [&] {
    if (armed) { runtime.Cancel(); }
    return ++clock;
  });
  check(temporary->WriteFile("DATA", std::array<uint8_t, 3>{1, 2, 3}) == 0, "read commit fixture failed");
  auto close = browser::InstallFileProvider(runtime, image, {}, temporary);
  browser::BindImageImports(image, runtime, {});
  X86Context context{}; context.host = &runtime; context.fs_base = 0x10800; context.esp = 0x10f00;
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    auto& memory = *image.memory;
    const auto put = [&](uint32_t at, uint32_t value) { memory.Write(ctx, at, &value, 4); };
    const auto get = [&](uint32_t at) { uint32_t value; memory.Read(ctx, at, &value, 4); return value; };
    const std::string path = "\\Device\\Harddisk0\\Partition1\\DATA";
    memory.Write(ctx, 0x10300, path.data(), path.size());
    put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(0x10204, 0x10300);
    put(0x10210, 0); put(0x10214, 0x10200); put(0x10218, 0x40);
    check(runtime.Call(ctx, base, runtime.KernelTarget(202), {0x10100, 0x80100080, 0x10210, 0x10108, 1, 0x60}) == 0,
      "read commit handle did not open");
    const auto handle = get(0x10100);
    const std::array<uint32_t, 9> frame{0, handle, 0, 0, 0, 0x10108, 0x10400, 3, 0};
    memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    put(0x10108, 0xabcdef); put(0x1010c, 0xabcdef); put(0x10400, 0xa5a5a5a5);
    const auto usage = temporary->Usage();
    const auto previous = temporary->Stat("DATA");
    bool cancelled = false;
    armed = true;
    try { runtime.Kernel(ctx, base, 219); }
    catch (const browser::Cancelled&) { cancelled = true; }
    check(!cancelled && ctx.eax == 0 && get(0x10108) == 0 && get(0x1010c) == 3 && get(0x10400) == 0xa5030201 &&
      temporary->Stat("DATA")->lastAccessTime == clock && temporary->Stat("DATA")->lastAccessTime > previous->lastAccessTime &&
      temporary->Stat("DATA")->lastWriteTime == previous->lastWriteTime && temporary->Usage().allocated == usage.allocated &&
      temporary->Usage().entries == usage.entries && close(handle), "read committed access metadata but cancelled before publishing bytes and count");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(runtime.ActiveWorkers() == 0, "read commit boundary retained worker");
};

const auto testWriteCancellation = [] {
  std::weak_ptr<browser::TemporaryFilesystem> lifetime;
  {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Map(0x10000, 4096); image.memory->Commit(0x10000, 4096);
    auto temporary = std::make_shared<browser::TemporaryFilesystem>(32768);
    lifetime = temporary;
    check(temporary->WriteFile("FILLED", std::array<uint8_t, 1>{1}) == 0 && temporary->CreateFile("TARGET", 4) == 0, "quota fixture failed");
    auto close = browser::InstallFileProvider(runtime, image, {}, temporary);
    image.image.kernel_thunk = 0x10000;
    browser::BindImageImports(image, runtime, {});
    std::atomic<bool> cancelled = false;
    X86Context context{}; context.host = &runtime; context.fs_base = 0x10800; context.esp = 0x10f00;
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      auto& memory = *image.memory;
      const auto put = [&](uint32_t at, uint32_t value) { memory.Write(ctx, at, &value, 4); };
      const auto get = [&](uint32_t at) { uint32_t value; memory.Read(ctx, at, &value, 4); return value; };
      const std::string name = "\\Device\\Harddisk0\\Partition1\\TARGET";
      memory.Write(ctx, 0x10300, name.data(), name.size());
      put(0x10200, uint32_t(name.size()) | (uint32_t(name.size()) << 16)); put(0x10204, 0x10300);
      put(0x10210, 0); put(0x10214, 0x10200); put(0x10218, 0x40);
      check(runtime.Call(ctx, base, runtime.KernelTarget(190), {0x10100, 0x40100080, 0x10210, 0x10108, 0, 0, 0, 5, 0x64}) == 0 && get(0x1010c) == 3,
        "actual overwrite quota fixture open failed");
      const auto writer = get(0x10100);
      put(0x10400, 0x76543210);
      check(runtime.Call(ctx, base, runtime.KernelTarget(236), {writer, 0, 0, 0, 0x10108, 0x10400, 4, 0}) == 0xc000007f && get(0x1010c) == 0 && temporary->Stat("TARGET")->size == 0, "disk-full write partially changed file");
      check(temporary->WriteFile("FILLED", {}) == 0, "quota release failed");
      check(runtime.Call(ctx, base, runtime.KernelTarget(236), {writer, 0, 0, 0, 0x10108, 0x10400, 4, 0}) == 0 && temporary->Stat("TARGET")->size == 4, "failed write advanced cursor");
      check(close(writer), "write fixture close failed");
      check(runtime.Call(ctx, base, runtime.KernelTarget(202), {0x10100, 0x100001, 0x10210, 0x10108, 3, 0x20}) == 0, "readonly fixture open failed");
      const auto reader = get(0x10100);
      check(runtime.Call(ctx, base, runtime.KernelTarget(236), {reader, 0, 0, 0, 0x10108, 0x10400, 4, 0}) == 0xc0000022 && close(reader), "readonly handle wrote bytes");
      check(runtime.Call(ctx, base, runtime.KernelTarget(236), {writer, 0, 0, 0, 0x10108, 0x10400, 4, 0}) == 0xc0000008, "closed writer remained live");
      check(runtime.Call(ctx, base, runtime.KernelTarget(202), {0x10100, 0x40100000, 0x10210, 0x10108, 3, 0x20}) == 0, "cancellation fixture open failed");
      const std::array<uint32_t, 9> frame = {0, get(0x10100), 0, 0, 0, 0x10108, 0x10400, 4, 0};
      memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      put(0x10108, 0xabcdef); put(0x1010c, 0xabcdef); put(0x10400, 0);
      runtime.Cancel();
      try { runtime.Kernel(ctx, base, 236); }
      catch (const browser::Cancelled&) { cancelled = true; }
      std::array<uint8_t, 4> bytes{};
      check(get(0x10108) == 0xabcdef && get(0x1010c) == 0xabcdef && temporary->Read("TARGET", 0, bytes) == 4 && bytes == std::array<uint8_t, 4>{0x10, 0x32, 0x54, 0x76}, "cancelled write changed bytes or outputs");
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(cancelled && runtime.ActiveWorkers() == 0, "cancelled write retained guest worker");
    close = {}; temporary.reset();
    check(!lifetime.expired(), "runtime lost live temporary backing");
  }
  check(lifetime.expired(), "write backing survived session teardown");
};

const auto testDataExportPreflight = [] {
  const std::array<X86FuncMapping, 1> mappings = {{{0x11000, entry}}};
  for (uint32_t n = 0; n < 5; ++n) {
    browser::Runtime runtime(mappings, {});
    browser::Runtime foreign(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    std::vector<browser::KernelDataExport> exports = {{n == 0 ? 326u : 12u, 0x1234, 4}};
    if (n == 1) { image.importsBound = true; }
    if (n == 2) { image.boundRuntime = &foreign; }
    if (n == 3) { runtime.Cancel(); }
    if (n == 4) { image.memory->Map(0xc0001000, 4096); }
    bool refused = false;
    try { browser::InstallFileProvider(runtime, image, {}, {}, &exports); }
    catch (const browser::Fault&) { refused = true; }
    catch (const browser::Cancelled&) { refused = true; }
    check(refused && exports.size() == 1 && exports[0].address == 0x1234 && (n == 3 || !runtime.KernelTarget(202)), "invalid data export installation published partial state");
  }
};

int main() {
  testDataExportPreflight();
  testReadCommitBoundary();
  testOverwriteStorage();
  testConcurrentOverwrite();
  testOverwriteCancellation();
  testWriteCancellation();
  testTemporaryMetadata();
  testTemporaryStorage();
  auto bytes = std::make_shared<std::vector<uint8_t>>(104448);
  const auto put = [&](uint32_t at, uint32_t value) { std::memcpy(bytes->data() + at, &value, 4); };
  std::memcpy(bytes->data() + 65536, "MICROSOFT*XBOX*MEDIA", 20);
  std::memcpy(bytes->data() + 65536 + 0x7ec, "MICROSOFT*XBOX*MEDIA", 20);
  put(65536 + 0x14, 40); put(65536 + 0x18, 64);
  put(81920 + 4, 50); put(81920 + 8, 6);
  (*bytes)[81920 + 13] = 11;
  std::memcpy(bytes->data() + 81920 + 14, "default.xbe", 11);
  std::memcpy(bytes->data() + 102400, "ISO123", 6);
  auto disc = std::make_shared<kernel::DiscIndex>();
  std::string error;
  check(disc->Open(bytes->size(), [bytes](uint64_t at, void* out, uint32_t size) { std::memcpy(out, bytes->data() + at, size); return size; }, error), "disc fixture invalid");
  testConcurrentProvider(disc);
  for (uint32_t n = 0; n < 3; ++n) {
    const std::array<X86FuncMapping, 1> mappings = {{{0x11000, entry}}};
    browser::Runtime runtime(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Map(0x10000, 4096); image.memory->Commit(0x10000, 4096);
    image.memory->Map(0x30000, 65536); image.memory->Commit(0x30000, 65536);
    image.image.entry = 0x11000;
    image.image.kernel_thunk = 0x10000;
    auto temporary = std::make_shared<browser::TemporaryFilesystem>();
    check(temporary->CreateDirectory("CACHE") == 0, "fixture directory failed");
    check(temporary->WriteFile("CACHE\\SAVE.BIN", std::array<uint8_t, 3>{1, 2, 3}) == 0, "fixture file failed");
    check(temporary->CreateFile("CACHE\\READONLY.BIN", 1) == 0, "readonly fixture file failed");
    std::vector<browser::KernelDataExport> exports;
    closeFile = browser::InstallFileProvider(runtime, image, disc, temporary, &exports);
    check(exports.size() == 2 && exports[0].ordinal == 40 && exports[0].address == 0xc0001000 && exports[0].size == 4 && exports[1].ordinal == 326 && exports[1].size == 8, "file data exports incorrect");
    uint32_t count = 0;
    image.memory->Read({}, exports[0].address, &count, 4);
    check(count == 1, "cache count does not match owned Partition3");
    std::array<uint32_t, 2> descriptor{};
    image.memory->Read({}, exports[1].address, descriptor.data(), sizeof(descriptor));
    const std::string expected = "\\Device\\CdRom0\\default.xbe";
    check(descriptor[0] == (expected.size() | ((expected.size() + 1) << 16)) && descriptor[1] >= 0xc0001000 && descriptor[1] + expected.size() + 1 <= 0xc0002000, "image name descriptor incorrect");
    std::vector<char> name(expected.size() + 1);
    image.memory->Read({}, descriptor[1], name.data(), name.size());
    check(std::string(name.data(), expected.size()) == expected && name.back() == 0, "image name bytes incorrect");
    browser::InstallThreadProvider(runtime, image, closeFile);
    browser::BindImageImports(image, runtime, {});
    X86Context host{}; host.host = &runtime; host.esp = 0x10800;
    bool refused = false;
    try { runtime.Kernel(host, image.memory->Base(), 202); } catch (const browser::Fault&) { refused = true; }
    check(refused && host.esp == 0x10800, "host caller admitted to file service");
    browser::ImageExecution execution;
    browser::ExecuteImageEntry(image, runtime, execution);
    check(execution.returned && runtime.ActiveWorkers() == 0, "file session leaked guest workers");
    std::array<uint8_t, 5> metadataBytes{};
    check(temporary->Read("UDATA\\1234ABCD\\TITLEMETA.XBX", 0, metadataBytes) == 5 && metadataBytes == std::array<uint8_t, 5>{4, 8, 12, 4, 8}, "successful guest writes were not retained exactly");
    check(temporary->Read("CACHE\\OVERWRITE.BIN", 0, metadataBytes) == 5 && metadataBytes == std::array<uint8_t, 5>{4, 8, 12, 4, 8},
      "actual write-only destination retained no real sequential bytes");
    check(!temporary->Stat("CACHE\\UNPUBLISHED.BIN"), "handle quota failure created actual destination backing");
    closeFile = {};
  }
  std::puts("file-provider-contracts-ok");
}
