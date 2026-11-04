#include "browser/file-provider.h"
#include "browser/cache-fatx.h"
#include "browser/thread-provider.h"
#include "browser/temporary-filesystem.h"
#include <array>
#include <chrono>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
std::function<bool(uint32_t)> closeFile;
uint32_t fileHandleLimit = 64;
const auto entry = [](X86Context& ctx, uint8_t* base) {
  constexpr uint32_t cacheHeaderSize = 4096;
  constexpr uint32_t cacheFatSize = ((browser::CacheFatx::Capacity / browser::CacheFatx::AllocationUnit + 1) * 2 + 4095) / 4096 * 4096;
  constexpr uint32_t cacheRootOffset = cacheHeaderSize + cacheFatSize;
  constexpr uint32_t cacheUnits = (browser::CacheFatx::Capacity - cacheRootOffset) / browser::CacheFatx::AllocationUnit;
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
  check(openExisting("D:\\default.xbe", 0x64, 1, 0xfffffffdu, 0x80100080, 1, 1) == 0,
    "Bink read-only file attributes rejected its existing ISO movie");
  const auto movie = get(0x10100);
  check(runtime.Call(ctx, base, runtime.KernelTarget(219), {movie, 0, 0, 0, 0x10840, 0x10900, 3, 0}) == 0 &&
    get(0x10844) == 3 && (get(0x10900) & 0xffffff) == 0x4f5349 && close(movie),
    "Bink existing-file mode did not retain bounded local ISO reads and handle retirement");
  check(openExisting("D:\\default.xbe", 0x48, 1, 0xfffffffdu, 0x80100080, 1, 0) == 0,
    "sound streaming asynchronous unbuffered open rejected its selected ISO");
  const auto stream = get(0x10100);
  put(0x10700, 3); put(0x10704, 0);
  check(runtime.Call(ctx, base, runtime.KernelTarget(219), {stream, 0, 0, 0, 0x10840, 0x10900, 3, 0x10700}) == 0 &&
    get(0x10840) == 0 && get(0x10844) == 3 && (get(0x10900) & 0xffffff) == 0x333231 && close(stream),
    "polling asynchronous read lost its explicit offset, bytes or completion status");
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
  check(deviceControl(0x70000, 24) == 0 && get(0x10a00) == browser::CacheFatx::Capacity / (512 * 32) && get(0x10a04) == 0 &&
    get(0x10a08) == 12 && get(0x10a0c) == 1 && get(0x10a10) == 32 && get(0x10a14) == 512, "virtual cache geometry not coherent");
  check(deviceControl(0x74004, 32) == 0 && get(0x10a00) == 0 && get(0x10a04) == 0 &&
    get(0x10a08) == browser::CacheFatx::Capacity && get(0x10a0c) == 0 && get(0x10a10) == 0 &&
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
    check(rawPartitionTransfer(ordinal, browser::CacheFatx::Capacity - 512, 1024) == 0xc000000d, "cache partition admitted overflow beyond capacity");
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
  std::vector<uint32_t> quotaLinks(fileHandleLimit - 1);
  for (uint32_t n = 0; n < quotaLinks.size(); ++n) {
    check(openLink("\\??\\D:") == 0, "bounded link quota admitted too few handles");
    quotaLinks[n] = get(0x10100);
  }
  check(openLink("\\??\\D:") == 0xc000009a, "file and link handles bypassed shared bounded quota");
  for (const auto handle : quotaLinks) { check(close(handle), "bounded link handle did not retire"); }
  check(close(quotaFile), "shared handle quota file did not retire");
  const auto deleteAlias = [&](std::string name) {
    memory.Write(ctx, 0x10300, name.data(), name.size());
    put(0x10200, uint32_t(name.size()) | (uint32_t(name.size()) << 16));
    put(0x10204, 0x10300);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(69), {0x10200});
    check(ctx.esp == stack, "symbolic link deletion corrupted stdcall cleanup");
    return result;
  };
  check(deleteAlias("\\??\\Q:") == 0xc0000034, "missing alias deletion fabricated success");
  check(createAlias("\\??\\Q:", "\\Device\\CdRom0") == 0, "deletion fixture alias creation failed");
  check(openLink("\\??\\Q:") == 0, "deletion fixture alias open failed");
  const auto retainedLink = get(0x10100);
  for (const std::string invalid : {"", "Q:", "\\??\\QQ:", "\\??\\1:", "\\Device\\CdRom0"}) {
    check(deleteAlias(invalid) == 0xc0000033, "malformed alias deletion was admitted");
  }
  check(deleteAlias("\\??\\q:") == 0 && deleteAlias("\\??\\Q:") == 0xc0000034,
    "alias deletion did not remove exactly its case-insensitive namespace entry");
  check(openLink("\\??\\Q:") == 0xc0000034, "deleted alias remained openable");
  check(queryLink(retainedLink) == 0 && close(retainedLink), "alias deletion invalidated an existing link object snapshot");
  check(createAlias("\\??\\Q:", "\\Device\\CdRom0") == 0 && deleteAlias("\\??\\Q:") == 0,
    "alias namespace could not be reused after deletion");
  check(createAlias("\\??\\D:", "\\Device\\CdRom0") == 0xc0000035, "observed existing disc alias did not collide truthfully");
  check(createAlias("\\??\\D:", "\\Device\\CdRom0") == 0xc0000035, "duplicate alias did not collide");
  check(createAlias("\\??\\X:", "\\Device\\Harddisk0\\Partition2") == 0xc00000bb, "alias mounted unknown device");
  check(createAlias("\\??\\X:", "\\Device\\Harddisk0\\Partition1\\..\\X") == 0xc0000033, "alias target traversal accepted");
  check(createAlias("\\??\\T:", "\\Device\\Harddisk0\\Partition1\\CACHE") == 0, "owned temporary alias failed");
  check(open("T:\\save.bin") == 0 && close(get(0x10100)), "temporary alias not bound to backed namespace");
  for (const std::string path : {"D:\\LEVELS\\", "\\??\\D:\\LEVELS\\", "\\Device\\CdRom0\\LEVELS\\",
      "T:\\", "\\Device\\Harddisk0\\Partition1\\CACHE\\"}) {
    check(open(path, 0xfffffffdu, 0x100001, 0x4021) == 0 && close(get(0x10100)),
      ("observed directory open refused single terminal separator after resolution: " + path).c_str());
  }
  check(open("D:\\", 0, 0x100001, 0x4021) == 0, "directory relative fixture failed");
  const auto levels = get(0x10100);
  check(open("LEVELS\\", levels, 0x100001, 0x4021) == 0 && close(get(0x10100)), "rooted directory terminal separator refused");
  check(close(levels), "directory relative fixture did not close");
  for (const std::string path : {"D:\\LEVELS\\\\", "D:\\LEVELS\\..\\", "D:\\LEVELS\\.\\", "D:\\\\LEVELS\\", "D:\\\\", "T:\\\\"}) {
    check(open(path, 0xfffffffdu, 0x100001, 0x4021) == 0xc0000033,
      "directory terminal normalization weakened separator or traversal checks");
  }
  for (const std::string path : {"D:\\default.xbe\\", "T:\\save.bin\\", "D:\\LEVELS\\"}) {
    check(open(path, 0xfffffffdu, 0x100001, 0x60) == 0xc0000033,
      "non-directory open gained terminal separator normalization");
  }
  check(open("D:\\default.xbe\\", 0xfffffffdu, 0x100001, 0x4021) == 0xc0000103,
    "normalized directory open accepted backed file");
  check(open("T:\\absent\\", 0xfffffffdu, 0x100001, 0x4021) == 0xc0000034,
    "directory normalization fabricated missing entry");
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
  const auto createFile = [&](std::string path, uint32_t disposition = 3, uint32_t output = 0x10100, uint32_t allocation = 0, uint32_t attributes = 4, uint32_t access = 0x40100000, uint32_t options = 0x22) {
    memory.Write(ctx, 0x10300, path.data(), path.size());
    put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(0x10204, 0x10300);
    put(0x10210, 0); put(0x10214, 0x10200); put(0x10218, 0x40);
    put(0x10100, 0xabcdef); put(0x10108, 0xabcdef); put(0x1010c, 0xabcdef);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(190), {output, access, 0x10210, 0x10108, allocation, attributes, 1, disposition, options});
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
  check(createFile(titleFile, 3, 0x10100, 0x10700) == 0 && queryFile(get(0x10100)) == 0 &&
    get(0x10828) == 5 && close(get(0x10100)), "existing OPEN_IF hint changed EOF");
  check(createFile("\\Device\\Harddisk0\\Partition1\\UDATA\\1234ABCD\\SaveMeta.xbx", 3, 0x10100, 0x10700, 4, 0xc0100000) == 0,
    "observed SaveMeta initial allocation request refused");
  const auto saveMeta = get(0x10100);
  check(queryFile(saveMeta) == 0 && get(0x10820) == 16384 && get(0x10828) == 0,
    "initial allocation changed logical EOF or vanished from metadata");
  check(writeFile(saveMeta) == 0 && queryFile(saveMeta) == 0 && get(0x10828) == 3 &&
    get(0x10820) == 16384 && close(saveMeta), "reserved metadata did not retain genuine writes");
  check(createFile("\\Device\\Harddisk0\\Partition1\\UDATA\\1234ABCD\\SaveMeta.xbx", 3, 0x10100, 0, 4, 0x80100080, 0x60) == 0,
    "observed readonly metadata OPEN_IF refused");
  const auto metadataReader = get(0x10100);
  check(runtime.Call(ctx, base, runtime.KernelTarget(219), {metadataReader, 0, 0, 0, 0x10840, 0x10a00, 3, 0}) == 0 &&
    (get(0x10a00) & 0xffffff) == (get(0x10900) & 0xffffff) && close(metadataReader), "metadata OPEN_IF lost stored bytes");
  check(createFile(titleFile, 5) == 0xc00000bb, "unobserved overwrite disposition accepted");
  for (const uint32_t options : {0x60u, 0x64u}) {
    const std::string profile = "\\Device\\Harddisk0\\Partition1\\UDATA\\1234ABCD\\PROFILE.SAV";
    check(createFile(profile, 3, 0x10100, 0, 0x80, 0xc0100080, 0x60) == 0 && close(get(0x10100)),
      "read-write OPEN_IF refused a writable temporary file");
    check(createFile(profile, 5, 0x10100, 0, 0x80, 0x40100080, options) == 0,
      "observed profile CREATE_ALWAYS mode refused");
    const auto handle = get(0x10100);
    check(writeFile(handle) == 0 && queryFile(handle) == 0 && get(0x10828) == 3 && close(handle),
      "profile CREATE_ALWAYS did not retain written bytes");
    check(createFile(profile, 3, 0x10100, 0, 0x80, 0x40100080, 0x60) == 0 && get(0x1010c) == 1 &&
      queryFile(get(0x10100)) == 0 && get(0x10828) == 3 && close(get(0x10100)),
      "observed profile write OPEN_IF refused or truncated existing bytes");
    check(createFile(profile, 3, 0x10100, 0, 0x80, 0xc0100080, 0x60) == 0 && get(0x1010c) == 1,
      "read-write OPEN_IF failed to preserve an existing file");
    const auto update = get(0x10100);
    check(runtime.Call(ctx, base, runtime.KernelTarget(219), {update, 0, 0, 0, 0x10840, 0x10a00, 3, 0}) == 0 &&
      (get(0x10a00) & 0xffffff) == (get(0x10900) & 0xffffff), "read-write OPEN_IF lost readable content");
    check(createFile(profile, 3, 0x10100, 0, 0x80, 0xc0100080, 0x60) == 0xc0000043,
      "read-write OPEN_IF ignored sharing restrictions");
    check(writeFile(update) == 0 && queryFile(update) == 0 && get(0x10828) == 6 && close(update),
      "read-write OPEN_IF did not preserve the cursor or write access");
    check(createFile(profile, 5, 0x10100, 0, 0x80, 0x40100080, options) == 0 &&
      get(0x1010c) == 3 && queryFile(get(0x10100)) == 0 && get(0x10828) == 0 && close(get(0x10100)),
      "profile overwrite did not atomically reset EOF");
  }
  check(createFile("D:\\default.xbe", 3, 0x10100, 0, 0x80, 0xc0100080, 0x60) == 0xc0000022,
    "read-write OPEN_IF admitted writes to the selected disc");
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
  check(rawPartitionTransfer(236, cacheHeaderSize) == 0, "guest FATX allocation table write failed");
  put(0x30000, 0);
  for (uint32_t n = 1; n < cacheFatSize / rawBytes.size(); ++n) {
    check(rawPartitionTransfer(236, cacheHeaderSize + n * rawBytes.size()) == 0, "guest FATX table tail write failed");
  }
  rawBytes.fill(0xff);
  memory.Write(ctx, 0x30000, rawBytes.data(), rawBytes.size());
  for (uint32_t n = 0; n < browser::CacheFatx::AllocationUnit / rawBytes.size(); ++n) {
    check(rawPartitionTransfer(236, cacheRootOffset + n * rawBytes.size()) == 0, "guest FATX root cluster write failed");
  }
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
  check(query(cacheRoot) == 0 && get(0x10400) == cacheUnits && get(0x10408) == cacheUnits - 1 && get(0x10410) * get(0x10414) == 16384, "cache quota includes reserved FATX bytes or wrong allocation unit");
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
  for (const std::string path : {"\\Device\\Harddisk0\\Partition3\\CACHE\\", "Z:\\CACHE\\"}) {
    check(open(path, 0xfffffffdu, 0x100001, 0x4021) == 0 && close(get(0x10100)),
      "mounted cache directory refused single terminal separator");
  }
  check(open("CACHE\\", cacheRoot, 0x100001, 0x4021) == 0, "cache directory relative terminal separator refused");
  const auto cacheDirectory = get(0x10100);
  for (const auto handle : {cacheRoot, cacheDirectory}) {
    check(runtime.Call(ctx, base, runtime.KernelTarget(207), {handle, 0, 0, 0, 0x10840, 0x10a00, 0x148, 1, 0, 0}) == 0 &&
      get(0x10840) == 0 && get(0x10a3c) == (handle == cacheRoot ? 5u : 8u) &&
      get(0x10a38) == (handle == cacheRoot ? 0x10u : 4u) && get(0x10a28) == (handle == cacheRoot ? 0u : 5u),
      "mounted cache enumeration did not use its real isolated metadata namespace");
  }
  check(close(cacheDirectory), "cache directory query fixture did not close");
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
  check(query(cacheRoot) == 0 && get(0x10408) == cacheUnits - 3, "cache namespace accounting does not reflect real cluster use");
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
  check(query(cacheRoot) == 0 && get(0x10408) == cacheUnits - 5, "cache destination bytes not charged to real volume");
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
  check(query(cacheRoot) == 0 && get(0x10408) == cacheUnits - 4, "overwrite did not release real allocation");
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
  check(seek(truncated, UINT64_MAX) == 0xc000000d && seek(truncated, 1, 7) == 0xc0000004 && seek(truncated, 1, 8, 21) == 0xc00000bb,
    "cache position admitted negative, short or unrelated set operation");
  for (uint32_t n = 0; n < 3; ++n) {
    bool refused = false;
    try { seek(truncated, 1, 8, 14, n == 0 ? 0x20000 : n == 1 ? 0x10840 : 0x10700, n == 2 ? 0x20000 : 0x10840); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && get(0x10840) == 0xabcdef, "bad cache position span partially published outputs");
  }
  check(seek(truncated, 2, 8, 20) == 0 && queryFile(truncated) == 0 && get(0x10828) == 2 &&
    seek(truncated, 12, 8, 20) == 0 && queryFile(truncated) == 0 && get(0x10828) == 12 &&
    seek(truncated, UINT64_MAX, 8, 20) == 0xc000000d && seek(truncated, 1, 7, 20) == 0xc0000004 &&
    seek(truncated, 5, 8, 20) == 0, "EOF updates failed or changed the current cursor");
  check(seek(truncated, 32768, 8, 19) == 0 && queryFile(truncated) == 0 &&
    get(0x10828) == 5 && get(0x10820) == 32768 && seek(truncated, 5, 8, 19) == 0 &&
    queryFile(truncated) == 0 && get(0x10828) == 5 && get(0x10820) == 16384,
    "allocation updates changed EOF or lost quota accounting");
  std::array<uint64_t, 5> basic{111, 222, 333, 444, 0x80};
  memory.Write(ctx, 0x10b00, basic.data(), sizeof(basic));
  const auto setBasic = [&](uint32_t handle, uint32_t length = 40) {
    return runtime.Call(ctx, base, runtime.KernelTarget(226), {handle, 0x10840, 0x10b00, length, 4});
  };
  check(setBasic(truncated) == 0 && setBasic(truncated, 39) == 0xc0000004 &&
    queryFile(truncated) == 0 && get(0x10800) == 111 && get(0x10808) == 222 && get(0x10810) == 333,
    "generic write did not grant basic metadata updates");
  check(writeFile(truncated, 0x10900, 1) == 0 && queryFile(truncated) == 0 && get(0x10828) == 6 && close(truncated),
    "failed cache position changed write cursor");
  check(open("Z:\\DEFAULT\\DYNAMICXBOX.UMD", 0xfffffffdu, 0x100100, 0x4040, 0x10100, 0x10108, 7) == 0,
    "original metadata-only asynchronous open refused");
  const auto attributesHandle = get(0x10100);
  check(setBasic(attributesHandle) == 0 && seek(attributesHandle, 1, 8, 20) == 0xc0000022 && close(attributesHandle),
    "metadata-only access changed data permissions");
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
  for (uint32_t options : {0x44u, 0x74u}) {
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
  check(seek(headerReader, 1, 8, 20) == 0xc0000022 && setBasic(headerReader) == 0xc0000022,
    "read-only handle acquired data or metadata write permission");

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
  {
    const std::string disposable = "\\Device\\Harddisk0\\Partition1\\CACHE\\DISPOSABLE.TMP";
    check(createFile(disposable) == 0 && close(get(0x10100)), "delete fixture creation failed");
    check(open(disposable, 0, 0x10000, 0x4040, 0x10100, 0x10108, 7) == 0, "observed temporary DELETE-only access was rejected");
    const auto deletion = get(0x10100);
    check(open(disposable, 0, 0x80100080, 0x60, 0x10100, 0x10108, 7) == 0, "shared deletion reader failed");
    const auto retained = get(0x10100);
    put(0x10b00, 1);
    check(runtime.Call(ctx, base, runtime.KernelTarget(226), {deletion, 0x10108, 0x10b00, 1, 13}) == 0,
      "temporary delete disposition rejected");
    check(open(disposable) == 0xc0000056 && close(deletion) && readFile(retained, 1) == 0xc0000011,
      "delete-pending file reopened or disappeared with a live handle");
    check(close(retained) && open(disposable) == 0xc0000034, "final close did not delete temporary file");
    check(open("D:\\default.xbe", 0, 0x110080, 0x60) == 0xc0000022, "DELETE access reached the ISO");
    check(createFile(disposable, 3, 0x10100, 0, 4, 0x40110000) == 0, "rename fixture creation failed");
    const auto renaming = get(0x10100);
    const std::string destination = "CACHE\\RENAMED.TMP";
    memory.Write(ctx, 0x10c00, destination.data(), destination.size());
    put(0x10b00, 0); put(0x10b04, temporaryRoot);
    put(0x10b08, uint32_t(destination.size()) | (uint32_t(destination.size()) << 16)); put(0x10b0c, 0x10c00);
    check(runtime.Call(ctx, base, runtime.KernelTarget(226), {renaming, 0x10108, 0x10b00, 16, 10}) == 0 &&
      writeFile(renaming) == 0 && close(renaming) && open(disposable) == 0xc0000034 &&
      open("\\Device\\Harddisk0\\Partition1\\CACHE\\RENAMED.TMP", 0, 0x80100080, 0x60) == 0,
      "rename failed to transfer namespace and live handle ownership");
    const auto renamed = get(0x10100);
    check(readFile(renamed, 3) == 0 && get(0x10844) == 3, "renamed handle writes lost their destination");
    put(0x10b00, 1);
    check(runtime.Call(ctx, base, runtime.KernelTarget(226), {renamed, 0x10108, 0x10b00, 1, 13}) == 0xc0000022 && close(renamed),
      "read-only handle acquired deletion rights");
  }
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
  std::vector<uint32_t> quotaFiles;
  for (uint32_t n = 0; n < fileHandleLimit; ++n) {
    check(open("D:\\default.xbe") == 0, "bounded file table filled early");
    quotaFiles.push_back(get(0x10100));
  }
  check(open("D:\\default.xbe") == 0xc000009a, "file handle capacity bypassed");
  check(overwrite("\\Device\\Harddisk0\\Partition1\\CACHE\\OVERWRITE.BIN", 0) == 0xc000009a &&
    overwrite("\\Device\\Harddisk0\\Partition1\\CACHE\\UNPUBLISHED.BIN", 0) == 0xc000009a,
    "actual overwrite bypassed shared handle quota");
  check(close(quotaFiles.back()), "file budget slot did not retire");
  check(overwrite("\\Device\\Harddisk0\\Partition1\\CACHE\\UNPUBLISHED.BIN", 0) == 0 &&
    close(get(0x10100)), "retired slot was not reusable for a new save destination");
  check(close(quotaFiles.front()), "file budget slot did not retire twice");
  check(openLink("\\??\\D:") == 0 && close(get(0x10100)), "retired file slot was not reusable by a link");
  for (uint32_t n = 1; n + 1 < quotaFiles.size(); ++n) { check(close(quotaFiles[n]), "file budget handle leaked"); }
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
    std::vector<browser::FileOpenDiagnostic> diagnostics;
    auto close = browser::InstallFileProvider(runtime, image, std::move(disc), temporary, nullptr, nullptr,
      [&](browser::FileOpenDiagnostic diagnostic) { diagnostics.push_back(std::move(diagnostic)); });
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
    check(diagnostics.size() == 64, "concurrent completed opens did not emit exactly once under provider serialization");
    for (const auto& diagnostic : diagnostics) {
      check(diagnostic.originalPath == "\\Device\\Harddisk0\\Partition1\\RACE" && diagnostic.resolvedPath == "RACE" &&
        diagnostic.status == 0 && diagnostic.root == 0 && diagnostic.disposition == 3 && diagnostic.options == 0x4021,
        "concurrent diagnostic lost owned path or frame fields");
    }
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
    check(cancelled && runtime.ActiveWorkers() == 0 && temporary->Usage().entries == 2 && diagnostics.size() == 64,
      "cancelled namespace operation retained workers, mutated backing or emitted diagnostic");
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

const auto testDirectoryQueries = [](std::shared_ptr<const kernel::DiscIndex> disc) {
  browser::Runtime runtime({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Map(0x10000, 8192); image.memory->Commit(0x10000, 8192);
  image.image.kernel_thunk = 0x10000;
  uint64_t time = 100;
  auto temporary = std::make_shared<browser::TemporaryFilesystem>(16 * 1024 * 1024, 256, [&] { return ++time; });
  check(temporary->CreateDirectory("DIR") == 0 && temporary->CreateDirectory("DIR\\SUB") == 0 &&
    temporary->CreateDirectory("EMPTY") == 0 && temporary->WriteFile("DIR\\B.TXT", std::array<uint8_t, 3>{1, 2, 3}) == 0 &&
    temporary->CreateFile("DIR\\A.BIN", 1) == 0 && temporary->CreateFile("DIR\\SUB\\NESTED.TXT", 0x80) == 0,
    "directory query temporary fixture failed");
  auto snapshot = temporary->List("dir");
  check(snapshot && snapshot->size() == 3 && (*snapshot)[0].name == "A.BIN" && (*snapshot)[0].metadata.attributes == 1 &&
    (*snapshot)[1].name == "B.TXT" && (*snapshot)[1].metadata.size == 3 && (*snapshot)[2].name == "SUB" &&
    (*snapshot)[2].metadata.directory && !temporary->List("DIR\\A.BIN") && !temporary->List("ABSENT") &&
    !temporary->List("DIR\\..") && temporary->List("EMPTY")->empty(), "metadata listing invented entries or exposed descendants");
  const auto original = (*snapshot)[1].metadata;
  check(temporary->WriteFile("DIR\\B.TXT", std::array<uint8_t, 4>{1, 2, 3, 4}) == 0 &&
    (*snapshot)[1].metadata.size == 3 && (*snapshot)[1].metadata.lastWriteTime == original.lastWriteTime,
    "temporary listing borrowed mutable backing");
  auto close = browser::InstallFileProvider(runtime, image, std::move(disc), temporary);
  browser::BindImageImports(image, runtime, {});
  X86Context context{}; context.host = &runtime; context.fs_base = 0x10800; context.esp = 0x11f00;
  X86Context retired{};
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    auto& memory = *image.memory;
    const auto put = [&](uint32_t at, uint32_t value) { memory.Write(ctx, at, &value, 4); };
    const auto get = [&](uint32_t at) { uint32_t value = 0; memory.Read(ctx, at, &value, 4); return value; };
    const auto get64 = [&](uint32_t at) { uint64_t value = 0; memory.Read(ctx, at, &value, 8); return value; };
    const auto open = [&](const std::string& path, uint32_t access = 0x100001, uint32_t options = 0x4021) {
      memory.Write(ctx, 0x10300, path.data(), path.size());
      put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(0x10204, 0x10300);
      put(0x10210, 0xfffffffdu); put(0x10214, 0x10200); put(0x10218, 0x40);
      const auto status = runtime.Call(ctx, base, runtime.KernelTarget(202), {0x10100, access, 0x10210, 0x10108, 3, options});
      check(status == 0, ("query fixture open failed: " + path + " access=" + std::to_string(access) +
        " options=" + std::to_string(options) + " status=" + std::to_string(status)).c_str());
      return get(0x10100);
    };
    const auto query = [&](uint32_t handle, const std::string& pattern = "", uint32_t restart = 0,
        uint32_t length = 0x148, uint32_t output = 0x11000, uint32_t statusOutput = 0x10180,
        uint32_t informationClass = 1, uint32_t event = 0, uint32_t apc = 0, uint32_t apcContext = 0,
        uint32_t descriptor = 0) {
      if (!pattern.empty()) {
        memory.Write(ctx, 0x10500, pattern.data(), pattern.size());
        put(0x10400, uint32_t(pattern.size()) | (uint32_t(pattern.size()) << 16)); put(0x10404, 0x10500);
        descriptor = 0x10400;
      }
      put(0x10180, 0xabcdef); put(0x10184, 0xabcdef);
      std::array<uint8_t, 0x148> guard; guard.fill(0x63); memory.Write(ctx, 0x11000, guard.data(), guard.size());
      const auto stack = ctx.esp;
      const auto result = runtime.Call(ctx, base, runtime.KernelTarget(207),
        {handle, event, apc, apcContext, statusOutput, output, length, informationClass, descriptor, restart});
      check(ctx.esp == stack && (statusOutput != 0x10180 || get(statusOutput) == result), "directory query status or ten-argument cleanup wrong");
      return result;
    };
    const auto name = [&] {
      std::string result(get(0x1103c), '\0'); memory.Read(ctx, 0x11040, result.data(), uint32_t(result.size())); return result;
    };
    const auto disk = open("D:\\"), independent = open("D:\\");
    check(query(disk, "*.X?E") == 0 && name() == "default.xbe" && get(0x10184) == 75 && get(0x11000) == 0 &&
      get(0x11038) == 0x80 && get64(0x11028) == 6 && get64(0x11030) == 4096 && get(0x1104b) == 0x63636363,
      "disc enumeration did not publish exact indexed file metadata/name/span");
    check(query(disk, "LEVELS") == 0x80000006 && get(0x10184) == 0 && get(0x11000) == 0x63636363,
      "subsequent mask replaced captured wildcard or end modified byte output");
    check(query(disk, "LEVELS", 1) == 0 && name() == "default.xbe", "restart replaced captured mask or did not rewind");
    check(query(independent, "LEVELS") == 0 && name() == "LEVELS" && get(0x11038) == 0x10 &&
      get64(0x11028) == 0 && get64(0x11030) == 0, "independent directory cursor or actual directory metadata wrong");
    const auto none = open("D:\\");
    check(query(none, "missing*") == 0xc000000f && get(0x10184) == 0 &&
      query(none) == 0x80000006 && query(none, "*", 1) == 0x80000006,
      "missing-match first/end/restart statuses wrong or wildcard not captured");
    const auto empty = open("D:\\LEVELS\\");
    check(query(empty) == 0xc000000f && query(empty) == 0x80000006, "actual empty directory did not report truthful exhaustion");
    const auto small = open("D:\\");
    check(query(small, "DEFAULT.XBE", 0, 63) == 0xc0000004 && get(0x10184) == 0 && get(0x11000) == 0x63636363,
      "short fixed-header query wrote output or captured cursor");
    check(query(small, "DEFAULT.XBE", 0, 66) == 0x80000005 && get(0x10184) == 66 && get(0x1103c) == 11 &&
      get(0x11040) == 0x63636564 && get(0x11044) == 0x63636363,
      "first filename overflow did not publish exact partial counted name");
    check(query(small, "", 0, 66) == 0 && get(0x10184) == 0 && get(0x11000) == 0x63636363,
      "subsequent short-buffer query modified output or advanced cursor");
    check(query(small) == 0 && name() == "default.xbe" && query(small) == 0x80000006,
      "overflow retry lost or duplicated real directory entry");
    const auto files = open("\\Device\\Harddisk0\\Partition1\\DIR\\");
    const auto files2 = open("\\Device\\Harddisk0\\Partition1\\DIR\\");
    check(query(files) == 0 && name() == "A.BIN" && get(0x11038) == 1 && get64(0x11008) != 0,
      "temporary snapshot omitted actual attributes/times");
    check(temporary->CreateFile("DIR\\AA.NEW", 0x80) == 0 && temporary->WriteFile("DIR\\B.TXT", std::array<uint8_t, 5>{1, 2, 3, 4, 5}) == 0,
      "temporary mutation fixture failed");
    check(query(files) == 0 && name() == "B.TXT" && get64(0x11028) == 4 && get64(0x11030) == 16384,
      "in-flight directory snapshot changed after backing mutation");
    check(query(files) == 0 && name() == "SUB" && get64(0x11030) == 16384 && query(files) == 0x80000006, "snapshot included late child or descendant");
    check(query(files, "", 1) == 0 && name() == "A.BIN" && query(files) == 0 && name() == "AA.NEW" &&
      query(files) == 0 && name() == "B.TXT" && get64(0x11028) == 5,
      "restart did not refresh real temporary child metadata");
    check(query(files2, "*.txt") == 0 && name() == "B.TXT" && query(files2) == 0x80000006,
      "temporary independent case-insensitive wildcard incorrect");
    const auto checks = open("D:\\");
    for (const uint32_t informationClass : {0u, 2u, 3u, 12u}) {
      check(query(checks, "", 0, 0x148, 0x11000, 0x10180, informationClass) == 0xc00000bb && get(0x10184) == 0,
        "unsupported directory information class accepted");
    }
    for (uint32_t n = 0; n < 3; ++n) {
      check(query(checks, "", 0, 0x148, 0x11000, 0x10180, 1, n == 0, n == 1, n == 2) == 0xc00000bb,
        "asynchronous event/APC directory query accepted");
    }
    check(query(checks, "bad\\name") == 0xc0000033 && query(checks, std::string(241, 'a')) == 0xc0000033 &&
      query(checks, "D:*") == 0xc0000033, "malformed wildcard descriptor accepted");
    for (uint32_t n = 0; n < 8; ++n) {
      bool refused = false;
      try {
        if (n == 7) { auto lease = memory.Borrow(); query(checks); }
        else {
          query(checks, "", 0, n == 5 ? UINT32_MAX : 0x148,
            n == 0 ? 0x20000 : n == 2 ? 0x10180 : n == 3 ? ctx.esp - 16 : n == 6 ? 0x10400 : 0x11000,
            n == 1 ? 0x20000 : n == 4 ? 0x10181 : 0x10180, 1, 0, 0, 0, n == 6 ? 0x10400 : 0);
        }
      } catch (const browser::Fault&) { refused = true; }
      check(refused && get(0x10180) == 0xabcdef && get(0x10184) == 0xabcdef && get(0x11000) == 0x63636363,
        "invalid directory output/input span partially published or failed to fault");
    }
    check(query(checks, "DEFAULT.XBE") == 0 && name() == "default.xbe",
      "refused queries captured mask, initialized snapshot or advanced cursor");
    check(query(checks, "", 0, 0x148, 0x11000, 0x10180, 1, 0, 0, 0, 0x20000) == 0x80000006,
      "ignored subsequent descriptor introduced guest read fault");
    check(query(0x70000004) == 0xc0000008 && get(0x10184) == 0, "foreign directory handle accepted");
    const auto file = open("D:\\default.xbe", 0x100001, 0x60);
    check(query(file) == 0xc0000024, "file handle accepted as directory");
    const auto noAccess = open("D:\\", 0x100080);
    check(query(noAccess) == 0xc0000022, "directory list access bypassed");
    const auto asynchronous = open("D:\\", 1, 1);
    check(query(asynchronous) == 0xc00000bb, "unsupported directory scheduling mode accepted");
    for (const auto handle : {disk, independent, none, empty, small, files, files2, checks, file, noAccess, asynchronous}) {
      check(close(handle) && query(handle) == 0xc0000008, "closed directory snapshot remained usable");
    }
    const auto cancelledHandle = open("D:\\");
    const std::array<uint32_t, 11> cancelledFrame{0, cancelledHandle, 0, 0, 0, 0x10180, 0x11000, 0x148, 1, 0, 0};
    memory.Write(ctx, ctx.esp, cancelledFrame.data(), sizeof(cancelledFrame));
    put(0x10180, 0xabcdef); put(0x10184, 0xabcdef); put(0x11000, 0x63636363);
    bool cancelled = false;
    runtime.Cancel();
    try { runtime.Kernel(ctx, base, 207); } catch (const browser::Cancelled&) { cancelled = true; }
    check(cancelled && get(0x10180) == 0xabcdef && get(0x10184) == 0xabcdef && get(0x11000) == 0x63636363 &&
      close(cancelledHandle), "cancelled directory query published outputs or retained handle");
    retired = ctx;
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(runtime.ActiveWorkers() == 0, "directory cancellation retained worker");
  bool refused = false;
  try { runtime.Kernel(retired, image.memory->Base(), 207); } catch (const browser::Fault&) { refused = true; }
  check(refused, "retired context retained directory-query access");
};

const auto testOpenDiagnostics = [] {
  for (uint32_t n = 0; n < 2; ++n) {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Map(0x10000, 4096); image.memory->Commit(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    auto temporary = std::make_shared<browser::TemporaryFilesystem>();
    check(temporary->CreateFile("TARGET", 0x80) == 0 && temporary->CreateDirectory("DIR") == 0, "diagnostic fixture failed");
    std::vector<browser::FileOpenDiagnostic> diagnostics;
    X86Context* active = nullptr;
    uint32_t completedStack = 0;
    auto close = browser::InstallFileProvider(runtime, image, {}, temporary, nullptr, nullptr,
      [&](browser::FileOpenDiagnostic diagnostic) {
        uint32_t status = 0, handle = 0, information = 0;
        image.memory->Read(*active, 0x10108, &status, 4);
        image.memory->Read(*active, 0x10100, &handle, 4);
        image.memory->Read(*active, 0x1010c, &information, 4);
        check(active->eax == diagnostic.status && status == diagnostic.status && active->esp == completedStack &&
          (status ? handle == 0xabcdef && information == 0 : handle != 0xabcdef && information == 1),
          "diagnostic observed incomplete guest publication");
        diagnostics.push_back(std::move(diagnostic));
        if (n) { throw std::runtime_error("diagnostic observer failure"); }
      });
    browser::BindImageImports(image, runtime, {});
    X86Context context{}; context.host = &runtime; context.fs_base = 0x10800; context.esp = 0x10f00;
    X86Context retired{};
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      active = &ctx;
      auto& memory = *image.memory;
      const auto put = [&](uint32_t at, uint32_t value) { memory.Write(ctx, at, &value, 4); };
      const auto get = [&](uint32_t at) { uint32_t value = 0; memory.Read(ctx, at, &value, 4); return value; };
      const auto open = [&](const std::string& path, bool create = false, uint32_t root = 0, uint32_t output = 0x10100,
          uint32_t descriptor = 0x10200, uint32_t options = 0x60) {
        memory.Write(ctx, 0x10300, path.data(), path.size());
        put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(0x10204, 0x10300);
        put(0x10210, root); put(0x10214, descriptor); put(0x10218, 0x40);
        put(0x10100, 0xabcdef); put(0x10108, 0xabcdef); put(0x1010c, 0xabcdef);
        completedStack = ctx.esp;
        return create ? runtime.Call(ctx, base, runtime.KernelTarget(190), {output, 0x80100080, 0x10210, 0x10108, 0, 0x80, 1, 1, options}) :
          runtime.Call(ctx, base, runtime.KernelTarget(202), {output, 0x80100080, 0x10210, 0x10108, 1, options});
      };
      const std::string path = "\\Device\\Harddisk0\\Partition1\\target";
      for (const bool create : {false, true}) {
        check(open(path, create) == 0 && close(get(0x10100)), "diagnostic changed successful open result");
        check(diagnostics.back().originalPath == path && diagnostics.back().resolvedPath == "TARGET" &&
          diagnostics.back().status == 0 && diagnostics.back().root == 0 && diagnostics.back().disposition == 1 &&
          diagnostics.back().options == 0x60, "diagnostic did not retain original case, resolution or frame");
      }
      const std::string directoryPath = "\\Device\\Harddisk0\\Partition1\\dir\\";
      check(open(directoryPath, false, 0xfffffffdu, 0x10100, 0x10200, 0x4021) == 0 && close(get(0x10100)) &&
        diagnostics.back().originalPath == directoryPath && diagnostics.back().resolvedPath == "DIR" &&
        diagnostics.back().status == 0 && diagnostics.back().options == 0x4021,
        "directory normalization diagnostic lost original separator or resolved backing");
      check(open("relative.movie", false, 0xfffffffdu) == 0xc0000033 && diagnostics.back().originalPath == "relative.movie" &&
        diagnostics.back().resolvedPath == "RELATIVE.MOVIE" && diagnostics.back().status == 0xc0000033 &&
        diagnostics.back().root == 0xfffffffdu, "invalid-name failure was not recorded truthfully");
      const std::string invalid = std::string("D:\\bad\0name", 11);
      check(open(invalid) == 0xc0000033 && diagnostics.back().originalPath == invalid,
        "diagnostic lost malformed counted name bytes");
      check(open(std::string(241, 'a')) == 0xc0000033 && diagnostics.back().originalPath.empty() &&
        diagnostics.back().resolvedPath.empty(), "rejected overlength descriptor was read for diagnostics");
      check(open(std::string(240, 'a')) == 0xc00000bb && diagnostics.back().originalPath == std::string(240, 'a') &&
        diagnostics.back().resolvedPath == std::string(240, 'A'), "diagnostic lost bounded input at exact limit");
      const std::string alias = "\\??\\T:", target = "\\Device\\Harddisk0\\Partition1\\" + std::string(200, 'a');
      memory.Write(ctx, 0x10400, target.data(), target.size()); memory.Write(ctx, 0x104f0, alias.data(), alias.size());
      put(0x10240, uint32_t(alias.size()) | (uint32_t(alias.size()) << 16)); put(0x10244, 0x104f0);
      put(0x10248, uint32_t(target.size()) | (uint32_t(target.size()) << 16)); put(0x1024c, 0x10400);
      check(runtime.Call(ctx, base, runtime.KernelTarget(67), {0x10240, 0x10248}) == 0, "diagnostic long alias fixture failed");
      const std::string aliasPath = "T:\\" + std::string(80, 'b');
      check(open(aliasPath) == 0xc0000033 && diagnostics.back().originalPath == aliasPath &&
        diagnostics.back().resolvedPath == std::string(200, 'A') + "\\" + std::string(39, 'B'),
        "expanded invalid-name diagnostic exceeded bound or changed return");
      const auto count = diagnostics.size();
      for (uint32_t fault = 0; fault < 3; ++fault) {
        bool refused = false;
        try {
          if (fault == 2) { auto lease = memory.Borrow(); open(path); }
          else { open(path, false, 0, fault == 0 ? 0x20000 : 0x10100, fault == 1 ? 0x20000 : 0x10200); }
        } catch (const browser::Fault&) { refused = true; }
        check(refused && diagnostics.size() == count && get(0x10100) == 0xabcdef && get(0x10108) == 0xabcdef &&
          get(0x1010c) == 0xabcdef, "faulting open emitted diagnostic or partially published");
      }
      check(diagnostics.size() == 8, "completed open emitted duplicate diagnostics");
      retired = ctx;
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    active = nullptr;
    bool refused = false;
    try { runtime.Kernel(retired, image.memory->Base(), 202); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && diagnostics.size() == 8, "retired context emitted diagnostic");
    for (const auto& diagnostic : diagnostics) {
      check(diagnostic.originalPath.size() <= 240 && diagnostic.resolvedPath.size() <= 240,
        "diagnostic path exceeded bound");
    }
    check(diagnostics[0].originalPath == "\\Device\\Harddisk0\\Partition1\\target" && diagnostics[0].resolvedPath == "TARGET",
      "diagnostic borrowed guest or provider path lifetime");
  }
};

const auto testPositionQueries = [](std::shared_ptr<const kernel::DiscIndex> disc) {
  browser::Runtime runtime({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Map(0x10000, 8192); image.memory->Commit(0x10000, 8192);
  image.image.kernel_thunk = 0x10000;
  uint64_t clock = 133000000000000000;
  auto temporary = std::make_shared<browser::TemporaryFilesystem>(65536, 8, [&] { return ++clock; });
  const std::array<uint8_t, 6> payload{1, 2, 3, 4, 5, 6};
  check(temporary->WriteFile("DATA", payload) == 0, "position-query temporary fixture failed");
  std::function<browser::DiscReadSnapshot()> discReads;
  auto close = browser::InstallFileProvider(runtime, image, std::move(disc), temporary, nullptr, &discReads);
  browser::BindImageImports(image, runtime, {});
  X86Context context{}; context.host = &runtime; context.fs_base = 0x10800; context.esp = 0x11f00;
  X86Context retired{};
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    auto& memory = *image.memory;
    const auto put = [&](uint32_t at, uint32_t value) { memory.Write(ctx, at, &value, 4); };
    const auto get = [&](uint32_t at) { uint32_t value = 0; memory.Read(ctx, at, &value, 4); return value; };
    const auto get64 = [&](uint32_t at) { uint64_t value = 0; memory.Read(ctx, at, &value, 8); return value; };
    const auto open = [&](const std::string& path, uint32_t access = 0x100001, uint32_t options = 0x20) {
      memory.Write(ctx, 0x10300, path.data(), path.size());
      put(0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); put(0x10204, 0x10300);
      put(0x10210, 0); put(0x10214, 0x10200); put(0x10218, 0x40);
      check(runtime.Call(ctx, base, runtime.KernelTarget(202), {0x10100, access, 0x10210, 0x10108, 3, options}) == 0,
        "position-query handle did not open");
      return get(0x10100);
    };
    const auto query = [&](uint32_t handle, uint32_t length = 8, uint32_t output = 0x10404, uint32_t status = 0x10500,
        uint32_t information = 14) {
      std::array<uint32_t, 16> guard; guard.fill(0xa5a5a5a5);
      memory.Write(ctx, 0x10400, guard.data(), sizeof(guard));
      put(0x10500, 0xabcdef); put(0x10504, 0xabcdef);
      const auto stack = ctx.esp;
      const auto result = runtime.Call(ctx, base, runtime.KernelTarget(211), {handle, status, output, length, information});
      check(ctx.esp == stack && get(status) == result && get(status + 4) == (result ? 0u : information == 34 ? 56u : 8u),
        "position-query IOSB or stdcall cleanup corrupt");
      check(get(0x10400) == 0xa5a5a5a5 && (result ? get64(0x10404) == 0xa5a5a5a5a5a5a5a5ull : true) &&
        (information != 14 || get(0x1040c) == 0xa5a5a5a5), "position-query wrote outside exact eight-byte result");
      return result;
    };
    const auto expect = [&](uint32_t handle, uint64_t position) {
      const auto status = query(handle);
      if (status || get64(0x10404) != position) {
        std::fprintf(stderr, "class14 cursor mismatch: status=%08x expected=%llu actual=%llu\n", status,
          static_cast<unsigned long long>(position), static_cast<unsigned long long>(get64(0x10404)));
      }
      check(status == 0 && get64(0x10404) == position, "class14 did not report actual handle cursor");
    };
    const auto seek = [&](uint32_t handle, uint64_t position) {
      memory.Write(ctx, 0x10600, &position, sizeof(position));
      check(runtime.Call(ctx, base, runtime.KernelTarget(226), {handle, 0x10500, 0x10600, 8, 14}) == 0 && get(0x10504) == 0,
        "position-query seek fixture failed");
    };
    const auto read = [&](uint32_t handle, uint32_t length, uint32_t offset = 0) {
      return runtime.Call(ctx, base, runtime.KernelTarget(219), {handle, 0, 0, 0, 0x10500, 0x10700, length, offset});
    };
    const auto write = [&](uint32_t handle, uint32_t length) {
      memory.Write(ctx, 0x10700, payload.data(), payload.size());
      return runtime.Call(ctx, base, runtime.KernelTarget(236), {handle, 0, 0, 0, 0x10500, 0x10700, length, 0});
    };
    const auto temporaryFile = open("\\Device\\Harddisk0\\Partition1\\DATA", 0xc0100000);
    const auto temporaryIndependent = open("\\Device\\Harddisk0\\Partition1\\DATA");
    const auto discFile = open("D:\\default.xbe");
    const auto discIndependent = open("\\??\\D:\\default.xbe");
    for (const auto handle : {temporaryFile, discFile}) {
      expect(handle, 0);
      check(read(handle, 2) == 0 && get(0x10504) == 2, "position-query real read failed");
      expect(handle, 2);
      expect(handle, 2);
      const auto sharedHandle = handle;
      check(read(sharedHandle, 1) == 0 && get(0x10504) == 1, "shared numeric handle did not consume real bytes");
      expect(handle, 3);
      expect(sharedHandle, 3);
      expect(handle == temporaryFile ? temporaryIndependent : discIndependent, 0);
      seek(handle, 5);
      expect(handle, 5);
      check(read(handle, 4) == 0 && get(0x10504) == 1, "position-query partial tail read failed");
      expect(handle, 6);
      check(read(handle, 1) == 0xc0000011 && get(0x10504) == 0, "position-query EOF fixture failed");
      expect(handle, 6);
      check(read(handle, 0) == 0 && get(0x10504) == 0, "position-query empty read failed");
      expect(handle, 6);
      for (const uint64_t position : {0x100000123ull, 0x7fffffffffffffffull}) {
        seek(handle, position);
        expect(handle, position);
        check(read(handle, 1) == 0xc0000011 && get(0x10504) == 0, "high cursor wrapped into backing bytes");
        expect(handle, position);
      }
      seek(handle, 2);
      for (const uint32_t length : {0u, 1u, 7u}) {
        check(query(handle, length) == 0xc0000004, "short class14 query succeeded");
        expect(handle, 2);
      }
      check(query(handle, 16) == 0 && get64(0x10404) == 2, "larger class14 span changed exact result");
      for (uint32_t n = 0; n < 13; ++n) {
        bool refused = false;
        const auto owner = ctx.host;
        const auto kpcr = ctx.fs_base;
        browser::Runtime foreign({}, {});
        try {
          if (n == 10) { auto lease = memory.Borrow(); query(handle); }
          else {
            if (n == 11) { ctx.host = &foreign; }
            if (n == 12) { ctx.fs_base ^= 4; }
            query(handle, n == 8 ? UINT32_MAX : 8,
              n == 0 ? 0x20000 : n == 2 ? 0x10405 : n == 4 ? 0x10504 : n == 5 ? ctx.esp - 4 : n == 7 ? 0x11ffc : n == 9 ? 0 : 0x10404,
              n == 1 ? 0x20000 : n == 3 ? 0x10501 : n == 6 ? ctx.esp - 4 : 0x10500);
          }
        } catch (const browser::Fault&) { refused = true; }
        ctx.host = owner; ctx.fs_base = kpcr;
        check(refused && get64(0x10404) == 0xa5a5a5a5a5a5a5a5ull && get(0x10500) == 0xabcdef && get(0x10504) == 0xabcdef,
          "invalid class14 span/ownership partially published outputs");
        expect(handle, 2);
      }
      check(read(handle, 1) == 0 && get(0x10504) == 1 && (get(0x10700) & 0xff) == (handle == discFile ? 'O' : 3),
        "class14 query consumed cursor or changed actual read bytes");
      expect(handle, 3);
    }
    seek(temporaryFile, 1);
    check(write(temporaryFile, 2) == 0 && get(0x10504) == 2, "position-query real write failed");
    expect(temporaryFile, 3);
    check(write(temporaryFile, 0) == 0, "position-query empty write failed");
    expect(temporaryFile, 3);
    check(write(discFile, 1) == 0xc0000022, "position-query changed disc write protection");
    expect(discFile, 3);
    const uint64_t explicitOffset = 1, currentOffset = UINT64_MAX - 1;
    memory.Write(ctx, 0x10600, &explicitOffset, 8);
    check(read(discFile, 2, 0x10600) == 0 && get(0x10504) == 2 && (get(0x10700) & 0xffff) == 0x4f53,
      "explicit disc read did not preserve indexed bytes");
    expect(discFile, 3);
    memory.Write(ctx, 0x10600, &currentOffset, 8);
    check(read(discFile, 1, 0x10600) == 0 && (get(0x10700) & 0xff) == '1', "FILE_CURRENT disc read lost actual cursor");
    expect(discFile, 4);
    const auto beforeMetadata = temporary->Stat("DATA");
    const auto beforeUsage = temporary->Usage();
    const auto beforeReads = discReads();
    const auto beforeClock = clock;
    for (uint32_t n = 0; n < 3; ++n) { expect(temporaryFile, 3); expect(discFile, 4); }
    const auto afterMetadata = temporary->Stat("DATA");
    const auto afterReads = discReads();
    check(clock == beforeClock && afterMetadata->size == beforeMetadata->size &&
      afterMetadata->lastAccessTime == beforeMetadata->lastAccessTime && afterMetadata->lastWriteTime == beforeMetadata->lastWriteTime &&
      afterMetadata->changeTime == beforeMetadata->changeTime && temporary->Usage().allocated == beforeUsage.allocated &&
      temporary->Usage().entries == beforeUsage.entries && afterReads.executableBytes == beforeReads.executableBytes &&
      afterReads.executableOperations == beforeReads.executableOperations && afterReads.assetBytes == beforeReads.assetBytes &&
      afterReads.assetOperations == beforeReads.assetOperations, "class14 query consumed I/O, storage or metadata");
    check(query(discFile, 56, 0x10404, 0x10500, 34) == 0 && get64(0x1042c) == 6 && get(0x10434) == 1,
      "class14 changed existing disc class34 metadata");
    check(query(temporaryFile, 56, 0x10404, 0x10500, 34) == 0 && get64(0x1042c) == 6,
      "class14 changed existing temporary class34 EOF");
    for (const uint32_t information : {19u, 20u, 5u}) {
      check(query(temporaryFile, 8, 0x10404, 0x10500, information) == 0xc00000bb, "class14 broadened unrelated query class");
    }
    const auto directory = open("D:\\", 0x100001, 0x21);
    const auto temporaryDirectory = open("\\Device\\Harddisk0\\Partition1", 0x100001, 0x21);
    const auto raw = open("\\Device\\Harddisk0\\Partition0", 0xc0100000, 0x10);
    for (const auto handle : {directory, temporaryDirectory, raw}) {
      check(query(handle) == 0xc00000bb && close(handle), "class14 broadened directory/raw position contract");
    }
    check(query(0x70000004) == 0xc0000008, "class14 accepted foreign handle");
    for (const auto handle : {temporaryFile, temporaryIndependent, discFile, discIndependent}) {
      check(close(handle) && query(handle) == 0xc0000008 && !close(handle), "closed class14 handle retained cursor");
    }
    const auto cancelledHandle = open("D:\\default.xbe");
    const std::array<uint32_t, 6> frame{0, cancelledHandle, 0x10500, 0x10404, 8, 14};
    memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    put(0x10404, 0xa5a5a5a5); put(0x10408, 0xa5a5a5a5); put(0x10500, 0xabcdef); put(0x10504, 0xabcdef);
    bool cancelled = false;
    runtime.Cancel();
    try { runtime.Kernel(ctx, base, 211); } catch (const browser::Cancelled&) { cancelled = true; }
    check(cancelled && get64(0x10404) == 0xa5a5a5a5a5a5a5a5ull && get(0x10500) == 0xabcdef && get(0x10504) == 0xabcdef && close(cancelledHandle),
      "cancelled class14 query partially published outputs");
    retired = ctx;
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  bool refused = false;
  try { runtime.Kernel(retired, image.memory->Base(), 211); } catch (const browser::Fault&) { refused = true; }
  check(refused && runtime.ActiveWorkers() == 0, "retired context retained position-query access");
  std::puts("file-position-query-contracts-ok");
};

const auto testPositionQueryReadRace = [](std::shared_ptr<const std::vector<uint8_t>> bytes) {
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Map(0x10000, 65536); image.memory->Commit(0x10000, 65536);
    image.memory->Map(0x30000, 4096); image.memory->Commit(0x30000, 4096);
    image.image.kernel_thunk = 0x10000;
    std::atomic<bool> reading = false, release = false, querying = false, queryDone = false;
    std::atomic<uint32_t> sharedHandle = 0, payloadReads = 0;
    auto disc = std::make_shared<kernel::DiscIndex>();
    std::string error;
    check(disc->Open(bytes->size(), [&, bytes](uint64_t at, void* output, uint32_t size) {
      if (at == 102400) {
        check(!browser::Memory::HasBorrow(), "disc I/O retained a guest backing lease");
        ++payloadReads;
        reading = true;
        while (!release) { std::this_thread::yield(); }
      }
      std::memcpy(output, bytes->data() + at, size);
      return size;
    }, error), "paused position-query disc fixture failed");
    std::weak_ptr<const kernel::DiscIndex> lifetime = disc;
    auto close = browser::InstallFileProvider(runtime, image, std::move(disc), {});
    browser::BindImageImports(image, runtime, {});
    X86Context reader{}; reader.host = &runtime; reader.fs_base = 0x18000; reader.esp = 0x13000;
    runtime.Launch(reader, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      auto& memory = *image.memory;
      const std::string path = "D:\\default.xbe";
      const std::array<uint32_t, 5> attributes{uint32_t(path.size()) | (uint32_t(path.size()) << 16), 0x11200, 0, 0x11000, 0x40};
      memory.Write(ctx, 0x11000, attributes.data(), sizeof(attributes));
      memory.Write(ctx, 0x11200, path.data(), path.size());
      check(runtime.Call(ctx, base, runtime.KernelTarget(202), {0x11020, 0x100001, 0x11008, 0x11040, 3, 0x20}) == 0,
        "paused reader open failed");
      uint32_t handle = 0;
      memory.Read(ctx, 0x11020, &handle, 4);
      sharedHandle = handle;
      const uint32_t sentinel = 0xa5a5a5a5;
      memory.Write(ctx, 0x11100, &sentinel, 4);
      const auto result = runtime.Call(ctx, base, runtime.KernelTarget(219), {handle, 0, 0, 0, 0x11040, 0x11100, 3, 0});
      std::array<uint32_t, 2> status{};
      memory.Read(ctx, 0x11040, status.data(), sizeof(status));
      uint32_t output = 0;
      memory.Read(ctx, 0x11100, &output, 4);
      check(result == (n == 1 ? 0xc0000008u : 0u) && status[0] == result && status[1] == (result ? 0u : 3u) &&
        output == (result ? sentinel : 0xa54f5349), "paused read lost close revalidation or real payload");
    });
    X86Context querier{}; querier.host = &runtime; querier.fs_base = 0x19000; querier.esp = 0x17000;
    runtime.Launch(querier, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      auto& memory = *image.memory;
      while (!reading) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
      const std::array<uint32_t, 2> sentinel{0xabcdef, 0xabcdef};
      memory.Write(ctx, 0x15040, sentinel.data(), sizeof(sentinel));
      memory.Write(ctx, 0x30000, sentinel.data(), sizeof(sentinel));
      querying = true;
      bool refused = false;
      uint32_t result = 0;
      try { result = runtime.Call(ctx, base, runtime.KernelTarget(211), {sharedHandle, 0x15040, 0x30000, 8, 14}); }
      catch (const browser::Fault&) { refused = true; }
      std::array<uint32_t, 2> status{};
      memory.Read(ctx, 0x15040, status.data(), sizeof(status));
      if (n == 2) {
        check(refused && status == sentinel, "unmapped waiting query partially published IOSB");
      } else {
        uint64_t position = 0;
        memory.Read(ctx, 0x30000, &position, sizeof(position));
        check(!refused && result == (n == 1 ? 0xc0000008u : 0u) && status[0] == result && status[1] == (result ? 0u : 8u) &&
          position == (result ? 0x00abcdef00abcdefull : 3ull), "waiting query published stale cursor or closed source");
      }
      queryDone = true;
    });
    while (!querying) { std::this_thread::yield(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!queryDone, "query did not serialize against actual paused disc read");
    {
      auto transition = image.memory->Mutate();
      check(!lifetime.expired(), "paused read/query lost pinned disc lifetime");
      if (n == 1) { check(close(sharedHandle), "paused disc handle did not close"); }
      if (n == 2) { image.memory->Decommit(0x30000, 4096); }
      release = true;
    }
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(queryDone && payloadReads == 1 && runtime.ActiveWorkers() == 0, "paused query deadlocked or repeated payload I/O");
    if (n != 1) { check(close(sharedHandle), "position-query shared handle did not close"); }
  }
  std::puts("file-position-query-races-ok");
};

const auto testNamedDiscFailure = [](std::shared_ptr<const std::vector<uint8_t>> bytes) {
  browser::Runtime runtime({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 8192);
  image.image.kernel_thunk = 0x10000;
  auto disc = std::make_shared<kernel::DiscIndex>();
  std::string error;
  check(disc->Open(bytes->size(), [bytes](uint64_t at, void* output, uint32_t size) {
    if (at == 102400) { return 0u; }
    std::memcpy(output, bytes->data() + at, size);
    return size;
  }, error), "failed-read fixture metadata invalid");
  browser::InstallFileProvider(runtime, image, disc);
  browser::BindImageImports(image, runtime, {});
  X86Context context{}; context.host = &runtime; context.esp = 0x11f00;
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    auto& memory = *image.memory;
    const std::string path = "D:\\default.xbe";
    const std::array<uint32_t, 5> attributes{uint32_t(path.size()) | (uint32_t(path.size()) << 16), 0x11200, 0, 0x11000, 0x40};
    memory.Write(ctx, 0x11000, attributes.data(), sizeof(attributes));
    memory.Write(ctx, 0x11200, path.data(), path.size());
    check(runtime.Call(ctx, base, runtime.KernelTarget(202), {0x11300, 0x100001, 0x11008, 0x11308, 3, 0x20}) == 0,
      "failed-read fixture open failed");
    uint32_t handle = 0;
    memory.Read(ctx, 0x11300, &handle, 4);
    const uint32_t sentinel = 0xabcdef;
    memory.Write(ctx, 0x11400, &sentinel, 4);
    bool named = false;
    try { runtime.Call(ctx, base, runtime.KernelTarget(219), {handle, 0, 0, 0, 0x11308, 0x11400, 3, 0}); }
    catch (const browser::Fault& failure) {
      const std::string message = failure.what();
      named = message.find("DEFAULT.XBE") != std::string::npos && message.find("offset=0") != std::string::npos && message.find("bytes=3") != std::string::npos;
    }
    uint32_t output = 0;
    memory.Read(ctx, 0x11400, &output, 4);
    check(named && output == sentinel, "failed streaming read lost its logical path or published partial bytes");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
};

const auto testLargeDiscReads = [](std::shared_ptr<const std::vector<uint8_t>> original) {
  constexpr uint32_t chunk = 1u << 20, length = 20 * chunk + 19, destination = 0x80020000;
  auto bytes = std::make_shared<std::vector<uint8_t>>(102400 + length);
  std::copy(original->begin(), original->end(), bytes->begin());
  std::memcpy(bytes->data() + 81920 + 8, &length, 4);
  (*bytes)[81920 + 13] = 9;
  std::memcpy(bytes->data() + 81920 + 14, "LARGE.SM2", 9);
  const auto pattern = [](uint32_t offset) { return uint8_t(offset * 37 + (offset >> 13)); };
  for (uint32_t n = 0; n < length; ++n) { (*bytes)[102400 + n] = pattern(n); }
  for (uint32_t n = 0; n < 3; ++n) {
    const uint32_t mode = n;
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 8192);
    image.image.kernel_thunk = 0x10000;
    uint32_t reads = 0, maximum = 0, handle = 0;
    bool named = false, cancelled = false;
    auto disc = std::make_shared<kernel::DiscIndex>();
    std::string error;
    check(disc->Open(bytes->size(), [&](uint64_t at, void* output, uint32_t size) {
      if (at >= 102400) {
        check(!browser::Memory::HasBorrow() && size <= chunk, "large read borrowed memory or exceeded bridge staging");
        ++reads; maximum = std::max(maximum, size);
        if (reads == 2 && mode == 1) { return 0u; }
        if (reads == 2 && mode == 2) { runtime.Cancel(); }
      }
      std::memcpy(output, bytes->data() + at, size);
      return size;
    }, error), "large read disc fixture failed");
    std::function<browser::DiscReadSnapshot()> snapshot;
    auto close = browser::InstallFileProvider(runtime, image, disc, {}, nullptr, &snapshot, {}, nullptr, {}, {}, 32 * chunk);
    browser::BindImageImports(image, runtime, {});
    X86Context context{}; context.host = &runtime; context.esp = 0x11800;
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      auto& memory = *image.memory;
      const std::string path = "D:\\large.sm2";
      const std::array<uint32_t, 5> attributes{uint32_t(path.size()) | uint32_t(path.size()) << 16, 0x11200, 0, 0x11000, 0x40};
      memory.Write(ctx, 0x11000, attributes.data(), sizeof(attributes));
      memory.Write(ctx, 0x11200, path.data(), path.size());
      check(runtime.Call(ctx, base, runtime.KernelTarget(202), {0x11300, 0x100001, 0x11008, 0x11308, 3, 0x20}) == 0,
        "large read open failed");
      memory.Read(ctx, 0x11300, &handle, 4);
      std::vector<uint8_t> sentinel(length, 0xa5);
      memory.Write(ctx, destination, sentinel.data(), sentinel.size());
      const std::array<uint32_t, 2> status{0xabcdef, 0xabcdef};
      memory.Write(ctx, 0x11308, status.data(), sizeof(status));
      try {
        check(runtime.Call(ctx, base, runtime.KernelTarget(219), {handle, 0, 0, 0, 0x11308, destination, length, 0}) == 0 && !mode,
          "bounded large disc read failed");
      } catch (const browser::Cancelled&) { cancelled = true; throw; }
      catch (const browser::Fault& failure) {
        const std::string detail = failure.what();
        named = detail.find("LARGE.SM2") != std::string::npos && detail.find("offset=1048576") != std::string::npos;
      }
      std::array<uint32_t, 2> delivered{};
      memory.Read(ctx, 0x11308, delivered.data(), sizeof(delivered));
      check(mode == 1 ? named && delivered == status : delivered == std::array<uint32_t, 2>{0, length},
        "large read published a partial result or lost its failing slice");
      check(runtime.Call(ctx, base, runtime.KernelTarget(211), {handle, 0x11308, 0x11400, 8, 14}) == 0,
        "large read cursor query failed");
      uint64_t position = 0;
      memory.Read(ctx, 0x11400, &position, 8);
      check(position == (mode ? 0 : length), "large read cursor advanced on failure");
    });
    if (const auto failure = runtime.Join()) {
      try { std::rethrow_exception(failure); }
      catch (const browser::Cancelled&) { check(mode == 2 && cancelled, "large read changed cancellation type"); }
    }
    std::vector<uint8_t> delivered(length);
    image.memory->Read({}, destination, delivered.data(), delivered.size());
    for (uint32_t n = 0; n < length; ++n) {
      check(delivered[n] == (mode ? 0xa5 : pattern(n)), "large read leaked partial or stale destination data");
    }
    check(reads == (mode ? 2u : 21u) && maximum == chunk && !runtime.ActiveWorkers() && close(handle),
      "large read did not bound chunks or retire its worker and file");
    check(snapshot().assetBytes == (mode ? 0 : length), "large read published fake asset progress");
  }
};

const auto testAsyncReads = [](std::shared_ptr<const kernel::DiscIndex> disc) {
  browser::Runtime runtime({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 8192);
  image.image.kernel_thunk = 0x10000;
  auto storage = std::make_shared<browser::TemporaryFilesystem>();
  const std::array<uint8_t, 6> content{'I', 'S', 'O', '1', '2', '3'};
  check(storage->WriteFile("CLIP.BIN", content) == 0, "asynchronous fixture creation failed");
  auto close = browser::InstallFileProvider(runtime, image, disc, storage);
  uint32_t begun = 0, completed = 0;
  uint32_t expectedStatus = 0, expectedCount = 3;
  image.beginFileIo = [&](uint32_t event) -> browser::FileIoCompletion {
    check(!browser::Memory::HasBorrow(), "file event began during a memory lease");
    if (event != 0x1234) { return {0xc0000008, {}}; }
    ++begun;
    return {0, [&] {
      std::array<uint32_t, 2> io{};
      image.memory->Read({}, 0x11308, io.data(), sizeof(io));
      check(io == std::array<uint32_t, 2>{expectedStatus, expectedCount}, "file event signaled before publishing its truthful IO status");
      if (expectedCount) {
        std::array<uint8_t, 3> bytes{};
        image.memory->Read({}, 0x11400, bytes.data(), bytes.size());
        check(bytes == std::array<uint8_t, 3>{'1', '2', '3'}, "file event signaled before publishing its actual bytes");
      }
      ++completed;
    }};
  };
  browser::BindImageImports(image, runtime, {});
  X86Context context{}; context.host = &runtime; context.esp = 0x11800;
  runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    auto& memory = *image.memory;
    for (const std::string path : {"D:\\default.xbe", "\\Device\\Harddisk0\\Partition1\\clip.bin"}) {
      const std::array<uint32_t, 5> attributes{uint32_t(path.size()) | uint32_t(path.size()) << 16, 0x11200, 0xfffffffdu, 0x11000, 0x40};
      memory.Write(ctx, 0x11000, attributes.data(), sizeof(attributes));
      memory.Write(ctx, 0x11200, path.data(), path.size());
      check(runtime.Call(ctx, base, runtime.KernelTarget(190), {0x11300, 0x80100080, 0x11008, 0x11308, 0, 0, 1, 1, 0x48}) == 0,
        "asynchronous read open failed for actual ISO or writable-volume backing");
      uint32_t handle = 0;
      memory.Read(ctx, 0x11300, &handle, 4);
      uint64_t offset = 3;
      memory.Write(ctx, 0x11500, &offset, 8);
      const auto read = [&](uint32_t event, uint32_t apc = 0, uint32_t offsetAddress = 0x11500, uint32_t completionContext = 0) {
        return runtime.Call(ctx, base, runtime.KernelTarget(219), {handle, event, apc, completionContext, 0x11308, 0x11400, 3, offsetAddress});
      };
      expectedStatus = 0; expectedCount = 3;
      check(read(0x1235, 0, 0x11500, 0x11308) == 0 && begun == completed,
        "overlapped read rejected its opaque context or low-bit completion-port flag");
      check(runtime.Call(ctx, base, runtime.KernelTarget(211), {handle, 0x11308, 0x11600, 8, 14}) == 0,
        "asynchronous read position query failed");
      uint64_t position = 1;
      memory.Read(ctx, 0x11600, &position, 8);
      check(position == 0, "explicit asynchronous read changed the shared synchronous file position");
      position = 2;
      memory.Write(ctx, 0x11600, &position, 8);
      check(runtime.Call(ctx, base, runtime.KernelTarget(226), {handle, 0x11308, 0x11600, 8, 14}) == 0 &&
        read(0) == 0 && runtime.Call(ctx, base, runtime.KernelTarget(211), {handle, 0x11308, 0x11600, 8, 14}) == 0,
        "asynchronous file rejected its independent SetFilePointer position");
      memory.Read(ctx, 0x11600, &position, 8);
      check(position == 2, "explicit overlapped read overwrote the independent file pointer");
      const auto before = completed;
      check(read(0x4321) == 0xc0000008 && read(0x1234, 1) == 0xc00000bb && read(0x1234, 0, 0) == 0xc00000bb &&
        completed == before && begun == before, "invalid asynchronous request reset or signaled a completion event");
      offset = 6;
      memory.Write(ctx, 0x11500, &offset, 8);
      expectedStatus = 0xc0000011; expectedCount = 0;
      check(read(0x1234) == expectedStatus && begun == completed, "asynchronous EOF did not publish and signal its actual status");
      check(close(handle), "asynchronous file handle did not retire");
    }
    const std::string destination = "\\Device\\Harddisk0\\Partition1\\overlap.bin";
    const std::array<uint32_t, 5> attributes{uint32_t(destination.size()) | uint32_t(destination.size()) << 16, 0x11200, 0xfffffffdu, 0x11000, 0x40};
    memory.Write(ctx, 0x11000, attributes.data(), sizeof(attributes));
    memory.Write(ctx, 0x11200, destination.data(), destination.size());
    check(runtime.Call(ctx, base, runtime.KernelTarget(190), {0x11300, 0x40100080, 0x11008, 0x11308, 0, 0, 1, 5, 0x48}) == 0,
      "sound duplication rejected its asynchronous overwrite destination");
    uint32_t handle = 0;
    memory.Read(ctx, 0x11300, &handle, 4);
    uint64_t offset = 3;
    memory.Write(ctx, 0x11500, &offset, 8);
    const std::array<uint8_t, 3> input{'1', '2', '3'};
    memory.Write(ctx, 0x11400, input.data(), input.size());
    expectedStatus = 0; expectedCount = 3;
    check(runtime.Call(ctx, base, runtime.KernelTarget(236), {handle, 0x1235, 0, 0x11308, 0x11308, 0x11400, 3, 0x11500}) == 0 &&
      begun == completed && storage->Stat("OVERLAP.BIN")->size == 6,
      "overlapped write did not retain its real completion and explicit offset");
    std::array<uint8_t, 6> written{};
    check(storage->Read("OVERLAP.BIN", 0, written) == 6 && written == std::array<uint8_t, 6>{0, 0, 0, '1', '2', '3'},
      "overlapped cache write lost its actual bytes or zero-filled gap");
    offset = storage->Usage().capacity;
    memory.Write(ctx, 0x11500, &offset, 8);
    expectedStatus = 0xc000007f; expectedCount = 0;
    check(runtime.Call(ctx, base, runtime.KernelTarget(236), {handle, 0x1234, 0, 0x11308, 0x11308, 0x11400, 3, 0x11500}) == expectedStatus &&
      begun == completed && storage->Stat("OVERLAP.BIN")->size == 6,
      "failed overlapped write fabricated bytes or lost its error completion");
    check(close(handle), "overlapped write handle did not retire");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(begun == 6 && completed == 6 && !runtime.ActiveWorkers(), "asynchronous file completion leaked or duplicated operations");
};

int main() {
  testOpenDiagnostics();
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
  put(81920, 8u << 16);
  put(81920 + 4, 50); put(81920 + 8, 6);
  (*bytes)[81920 + 13] = 11;
  std::memcpy(bytes->data() + 81920 + 14, "default.xbe", 11);
  (*bytes)[81920 + 32 + 12] = 0x10; (*bytes)[81920 + 32 + 13] = 6;
  std::memcpy(bytes->data() + 81920 + 32 + 14, "LEVELS", 6);
  std::memcpy(bytes->data() + 102400, "ISO123", 6);
  auto disc = std::make_shared<kernel::DiscIndex>();
  std::string error;
  check(disc->Open(bytes->size(), [bytes](uint64_t at, void* out, uint32_t size) { std::memcpy(out, bytes->data() + at, size); return size; }, error), "disc fixture invalid");
  testNamedDiscFailure(bytes);
  testLargeDiscReads(bytes);
  testAsyncReads(disc);
  testPositionQueries(disc);
  testPositionQueryReadRace(bytes);
  testConcurrentProvider(disc);
  testDirectoryQueries(disc);
  for (uint32_t n = 0; n < 3; ++n) {
    fileHandleLimit = n == 2 ? 256 : 64;
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
    std::vector<browser::FileOperationDiagnostic> operationDiagnostics;
    closeFile = browser::InstallFileProvider(runtime, image, disc, temporary, &exports, nullptr, {}, nullptr, {},
      [&](browser::FileOperationDiagnostic diagnostic) { operationDiagnostics.push_back(std::move(diagnostic)); }, 1u << 20, fileHandleLimit);
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
    check(operationDiagnostics.size() <= 64 && std::any_of(operationDiagnostics.begin(), operationDiagnostics.end(), [](const auto& diagnostic) {
      return diagnostic.ordinal == 226 && diagnostic.informationClass == 13 && diagnostic.status == 0xc0000022 &&
        diagnostic.path == "CACHE\\RENAMED.TMP" && diagnostic.length == 1;
    }), "recoverable metadata failure lost its bounded named operation diagnostic");
    check(std::any_of(operationDiagnostics.begin(), operationDiagnostics.end(), [](const auto& diagnostic) {
      return (diagnostic.ordinal == 219 || diagnostic.ordinal == 236) && !diagnostic.path.empty() &&
        diagnostic.status && diagnostic.status != 0xc0000011;
    }), "recoverable transfer failure lost its named operation diagnostic");
    std::array<uint8_t, 5> metadataBytes{};
    check(temporary->Read("UDATA\\1234ABCD\\TITLEMETA.XBX", 0, metadataBytes) == 5 && metadataBytes == std::array<uint8_t, 5>{4, 8, 12, 4, 8}, "successful guest writes were not retained exactly");
    check(temporary->Read("CACHE\\OVERWRITE.BIN", 0, metadataBytes) == 5 && metadataBytes == std::array<uint8_t, 5>{4, 8, 12, 4, 8},
      "actual write-only destination retained no real sequential bytes");
    check(temporary->Stat("CACHE\\UNPUBLISHED.BIN")->size == 0, "reused save slot retained unexpected data");
    closeFile = {};
  }
  std::puts("file-provider-contracts-ok");
}
