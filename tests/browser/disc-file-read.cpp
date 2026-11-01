#include "browser/file-provider.h"
#include <array>
#include <atomic>
#include <cstdio>

namespace {
const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
struct Fixture {
  browser::Runtime runtime{{}, {}};
  browser::PreparedImage image;
  std::shared_ptr<std::vector<uint8_t>> bytes = std::make_shared<std::vector<uint8_t>>(104448);
  std::shared_ptr<kernel::DiscIndex> disc = std::make_shared<kernel::DiscIndex>();
  std::shared_ptr<browser::TemporaryFilesystem> temporary = std::make_shared<browser::TemporaryFilesystem>();
  std::function<bool(uint32_t)> close;
  std::function<browser::DiscReadSnapshot()> snapshot;
  std::atomic<uint32_t> calls = 0;
  std::function<uint32_t(uint64_t, void*, uint32_t)> reader;
  Fixture() {
    const auto put = [&](uint32_t at, uint32_t value) { std::memcpy(bytes->data() + at, &value, 4); };
    std::memcpy(bytes->data() + 65536, "MICROSOFT*XBOX*MEDIA", 20);
    std::memcpy(bytes->data() + 65536 + 0x7ec, "MICROSOFT*XBOX*MEDIA", 20);
    put(65536 + 0x14, 40); put(65536 + 0x18, 64);
    (*bytes)[81920 + 2] = 7;
    put(81920 + 4, 50); put(81920 + 8, 7);
    (*bytes)[81920 + 13] = 11;
    std::memcpy(bytes->data() + 81920 + 14, "default.xbe", 11);
    put(81920 + 28 + 4, 51); put(81920 + 28 + 8, 7);
    (*bytes)[81920 + 28 + 13] = 9;
    std::memcpy(bytes->data() + 81920 + 28 + 14, "story.dat", 9);
    bytes->resize(106496);
    for (uint32_t n = 0; n < 7; ++n) { (*bytes)[102400 + n] = uint8_t(10 + n); }
    for (uint32_t n = 0; n < 7; ++n) { (*bytes)[104448 + n] = uint8_t(20 + n); }
    reader = [&](uint64_t at, void* out, uint32_t size) {
      check(!browser::Memory::HasBorrow(), "backing lease held during disc I/O");
      std::memcpy(out, bytes->data() + at, size);
      return size;
    };
    std::string error;
    check(disc->Open(bytes->size(), [&](uint64_t at, void* out, uint32_t size) {
      if (at >= 102400) { ++calls; }
      return reader(at, out, size);
    }, error), "invalid disc fixture");
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 0x10000);
    image.memory->Ensure(0x30000, (1u << 20) + 4096);
    image.image.kernel_thunk = 0x10000;
    close = browser::InstallFileProvider(runtime, image, disc, temporary, nullptr, &snapshot);
    browser::BindImageImports(image, runtime, {});
  }
  void Run(const std::function<void(X86Context&, uint8_t*)>& entry) {
    X86Context context{}; context.host = &runtime; context.fs_base = 0x18000; context.esp = 0x1f000;
    runtime.Launch(context, image.memory->Base(), entry);
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
  void Put(X86Context& ctx, uint32_t at, uint32_t value) { image.memory->Write(ctx, at, &value, 4); }
  uint32_t Get(X86Context& ctx, uint32_t at) { uint32_t value; image.memory->Read(ctx, at, &value, 4); return value; }
  uint32_t Open(X86Context& ctx, uint8_t* base, const std::string& name = "D:\\story.dat", uint32_t access = 0x100001, uint32_t options = 0x20, uint32_t expected = 0) {
    image.memory->Write(ctx, 0x10300, name.data(), name.size());
    Put(ctx, 0x10200, uint32_t(name.size()) * 0x10001); Put(ctx, 0x10204, 0x10300);
    Put(ctx, 0x10210, 0); Put(ctx, 0x10214, 0x10200); Put(ctx, 0x10218, 0x40);
    check(runtime.Call(ctx, base, runtime.KernelTarget(202), {0x10100, access, 0x10210, 0x10108, 3, options}) == expected, "disc fixture handle open failed");
    return Get(ctx, 0x10100);
  }
  uint32_t Read(X86Context& ctx, uint8_t* base, uint32_t handle, uint32_t length, uint32_t offset = 0,
      uint32_t destination = 0x30000, uint32_t status = 0x10800, uint32_t event = 0, uint32_t apc = 0, uint32_t apcContext = 0) {
    Put(ctx, 0x10800, 0xabcdef); Put(ctx, 0x10804, 0xabcdef); Put(ctx, 0x30000, 0xa5a5a5a5);
    const auto stack = ctx.esp;
    const auto result = runtime.Call(ctx, base, runtime.KernelTarget(219), {handle, event, apc, apcContext, status, destination, length, offset});
    check(ctx.esp == stack && Get(ctx, status) == result, "read frame cleanup or status mismatch");
    return result;
  }
};

void UnbufferedAdvisory() {
  Fixture fixture;
  check(fixture.temporary->WriteFile("DATA", std::array<uint8_t, 3>{1, 2, 3}) == 0, "advisory temporary fixture failed");
  fixture.Run([&](X86Context& ctx, uint8_t* base) {
    const auto open = [&](const std::string& path = "D:\\story.dat", uint32_t options = 0x6c, uint32_t access = 0x80100080,
        uint32_t output = 0x10100, uint32_t status = 0x10108, uint32_t attributes = 0x80, uint32_t allocation = 0) {
      fixture.image.memory->Write(ctx, 0x10300, path.data(), path.size());
      fixture.Put(ctx, 0x10200, uint32_t(path.size()) | (uint32_t(path.size()) << 16)); fixture.Put(ctx, 0x10204, 0x10300);
      fixture.Put(ctx, 0x10210, 0xfffffffdu); fixture.Put(ctx, 0x10214, 0x10200); fixture.Put(ctx, 0x10218, 0x40);
      fixture.Put(ctx, 0x10100, 0xabcdef); fixture.Put(ctx, 0x10108, 0xabcdef); fixture.Put(ctx, 0x1010c, 0xabcdef);
      const auto stack = ctx.esp;
      const auto result = fixture.runtime.Call(ctx, base, fixture.runtime.KernelTarget(190), {output, access, 0x10210, status, allocation, attributes, 1, 1, options});
      check(ctx.esp == stack && fixture.Get(ctx, status) == result && fixture.Get(ctx, status + 4) == (result ? 0u : 1u), "observed6C stdcall or create status corrupt");
      return result;
    };
    check(open() == 0, "actual readonly disc FILE_OPEN options6C refused virtual no-provider-cache advisory");
    const auto direct = fixture.Get(ctx, 0x10100);
    check(fixture.Read(ctx, base, direct, 3) == 0 && fixture.Get(ctx, 0x30000) == 0xa5161514 && fixture.calls == 1,
      "virtual advisory read failed genuine non-sector-sized source extent");
    fixture.Put(ctx, 0x10900, 0); fixture.Put(ctx, 0x10904, 0);
    check(fixture.runtime.Call(ctx, base, fixture.runtime.KernelTarget(226), {direct, 0x10800, 0x10900, 8, 14}) == 0 &&
      fixture.Read(ctx, base, direct, 3) == 0 && fixture.Get(ctx, 0x30000) == 0xa5161514 && fixture.calls == 2,
      "virtual direct advisory retained provider data cache instead of invoking real reader again");
    check(open() == 0, "shared readonly advisory handle did not open");
    const auto independent = fixture.Get(ctx, 0x10100);
    check(fixture.Read(ctx, base, independent, 1) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a514 && fixture.close(independent),
      "advisory handle reused independent handle cursor");
    check(fixture.runtime.Call(ctx, base, fixture.runtime.KernelTarget(236), {direct, 0, 0, 0, 0x10800, 0x30000, 1, 0}) == 0xc0000022,
      "virtual no-provider-cache advisory admitted ISO write");
    check(open("D:\\story.dat", 0x6c, 0x40100080) != 0 && open("D:\\story.dat", 0x6c, 0xc0100080) != 0 &&
      open("\\Device\\Harddisk0\\Partition1\\DATA") == 0xc00000bb && open("D:\\story.dat", 0x6d) != 0,
      "observed readonly advisory expanded into writes, owned cache or conflicting directory mode");
    check(open("D:\\missing") == 0xc0000034 && open("D:\\missing\\child") == 0xc000003a && open("D:\\..\\story.dat") == 0xc0000033,
      "virtual advisory fabricated paths or lost source namespace failures");
    fixture.Put(ctx, 0x10900, 1); fixture.Put(ctx, 0x10904, 0);
    check(open("D:\\story.dat", 0x6c, 0x80100080, 0x10100, 0x10108, 0x80, 0x10900) == 0xc00000bb &&
      open("D:\\story.dat", 0x6c, 0x80100080, 0x10100, 0x10108, 1) == 0xc00000bb,
      "advisory admitted unobserved allocation or readonly-create attributes");
    for (uint32_t n = 0; n < 3; ++n) {
      bool refused = false;
      try { open("D:\\story.dat", 0x6c, 0x80100080, n == 0 ? 0x20000 : n == 1 ? 0x10108 : 0x10100, n == 2 ? 0x20000 : 0x10108); }
      catch (const browser::Fault&) { refused = true; }
      check(refused && fixture.Get(ctx, 0x10100) == 0xabcdef && fixture.Get(ctx, 0x10108) == 0xabcdef,
        "bad advisory open spans published partial output or handle");
    }
    check(fixture.Read(ctx, base, direct, 1) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a517 && fixture.calls == 4 && fixture.close(direct),
      "failed advisory opens changed live cursor or performed unrequested source I/O");
    const std::array<uint32_t, 10> frame{0, 0x10100, 0x80100080, 0x10210, 0x10108, 0, 0x80, 1, 1, 0x6c};
    fixture.image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    fixture.Put(ctx, 0x10100, 0xabcdef); fixture.Put(ctx, 0x10108, 0xabcdef); fixture.Put(ctx, 0x1010c, 0xabcdef);
    fixture.runtime.Cancel();
    bool cancelled = false;
    try { fixture.runtime.Kernel(ctx, base, 190); }
    catch (const browser::Cancelled&) { cancelled = true; }
    check(cancelled && fixture.Get(ctx, 0x10100) == 0xabcdef && fixture.Get(ctx, 0x10108) == 0xabcdef &&
      fixture.Get(ctx, 0x1010c) == 0xabcdef && fixture.calls == 4, "cancelled advisory open published outputs or touched source bytes");
  });
}

void PositionAndMetadata() {
  Fixture fixture;
  fixture.Run([&](X86Context& ctx, uint8_t* base) {
    const auto file = fixture.Open(ctx, base);
    const auto query = [&](uint32_t length = 56, uint32_t information = 34, uint32_t output = 0x10a00, uint32_t status = 0x10800) {
      for (uint32_t n = 0; n < 14; ++n) { fixture.Put(ctx, 0x10a00 + n * 4, 0xabcdef); }
      fixture.Put(ctx, 0x10800, 0xabcdef); fixture.Put(ctx, 0x10804, 0xabcdef);
      const auto stack = ctx.esp;
      const auto result = fixture.runtime.Call(ctx, base, fixture.runtime.KernelTarget(211), {file, status, output, length, information});
      check(ctx.esp == stack && fixture.Get(ctx, status) == result && fixture.Get(ctx, status + 4) == (result ? 0u : 56u), "disc metadata status or cleanup corrupt");
      return result;
    };
    check(query() == 0 && fixture.Get(ctx, 0x10a20) == 4096 && fixture.Get(ctx, 0x10a24) == 0 && fixture.Get(ctx, 0x10a28) == 7 &&
      fixture.Get(ctx, 0x10a2c) == 0 && fixture.Get(ctx, 0x10a30) == 1 && fixture.Get(ctx, 0x10a34) == 0 && fixture.calls == 0,
      "actual ISO network-open metadata is not retained source extent and read-only attributes");
    for (uint32_t n = 0; n < 8; ++n) { check(fixture.Get(ctx, 0x10a00 + n * 4) == 0, "unspecified virtual ISO timestamps claimed recovered console metadata"); }
    check(query(55) == 0xc0000004 && fixture.Get(ctx, 0x10a00) == 0xabcdef && query(56, 5) == 0xc00000bb, "disc metadata accepted wrong length or speculative class");
    for (uint32_t n = 0; n < 3; ++n) {
      bool refused = false;
      try { query(56, 34, n == 0 ? 0x20000 : n == 1 ? 0x10800 : 0x10a00, n == 2 ? 0x10a04 : 0x10800); }
      catch (const browser::Fault&) { refused = true; }
      check(refused && fixture.Get(ctx, 0x10800) == 0xabcdef && fixture.Get(ctx, 0x10a00) == 0xabcdef, "disc metadata bad span partially published");
    }
    const auto seek = [&](uint64_t position, uint32_t handle, uint32_t length = 8, uint32_t information = 14, uint32_t input = 0x10900, uint32_t status = 0x10800) {
      fixture.Put(ctx, 0x10900, uint32_t(position)); fixture.Put(ctx, 0x10904, uint32_t(position >> 32));
      fixture.Put(ctx, 0x10800, 0xabcdef); fixture.Put(ctx, 0x10804, 0xabcdef);
      const auto stack = ctx.esp;
      const auto result = fixture.runtime.Call(ctx, base, fixture.runtime.KernelTarget(226), {handle, status, input, length, information});
      check(ctx.esp == stack && fixture.Get(ctx, status) == result && fixture.Get(ctx, status + 4) == 0, "position stdcall, status or information corrupt");
      return result;
    };
    check(seek(2, file) == 0 && fixture.Read(ctx, base, file, 2) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a51716, "source seek did not update synchronous read cursor");
    check(query() == 0 && fixture.Read(ctx, base, file, 1) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a518, "source metadata query moved cursor");
    check(seek(0x100000000ull, file) == 0 && fixture.Read(ctx, base, file, 1) == 0xc0000011 && seek(0, file) == 0,
      "large positive source seek wrapped or could not return to start");
    check(seek(UINT64_MAX, file) == 0xc000000d && seek(UINT64_MAX - 1, file) == 0xc000000d, "position accepted negative or read-offset sentinel");
    check(seek(4, file, 7) == 0xc0000004 && seek(4, file, 9) == 0xc0000004 && seek(4, file, 8, 20) == 0xc00000bb,
      "position accepted wrong input extent or speculative set class");
    const auto async = fixture.Open(ctx, base, "D:\\story.dat", 0x100001, 0);
    const auto directory = fixture.Open(ctx, base, "D:\\", 0x100001, 0x21);
    check(seek(1, async) == 0xc00000bb && seek(1, directory) == 0xc00000ba && seek(1, 0x70000004) == 0xc0000008,
      "position accepted async, directory or foreign handle");
    for (uint32_t n = 0; n < 5; ++n) {
      bool refused = false;
      try { seek(6, file, 8, 14, n == 0 ? 0x20000 : n == 1 ? 0x10804 : 0x10900, n == 2 ? 0x20000 : n == 3 ? 0x10904 : n == 4 ? 0x10801 : 0x10800); }
      catch (const browser::Fault&) { refused = true; }
      check(refused && fixture.Get(ctx, 0x10800) == 0xabcdef, "invalid position partially published status");
    }
    check(fixture.Read(ctx, base, file, 1) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a514, "failed seek advanced source cursor");
    check(fixture.close(file) && seek(0, file) == 0xc0000008, "closed source position remained live");
  });
}

void Contracts() {
  Fixture fixture;
  check(fixture.snapshot().assetBytes == 0 && fixture.snapshot().executableBytes == 0, "parser reads counted as guest delivery");
  fixture.Run([&](X86Context& ctx, uint8_t* base) {
    const auto file = fixture.Open(ctx, base);
    check(fixture.Read(ctx, base, file, 3) == 0 && fixture.Get(ctx, 0x10804) == 3 && fixture.Get(ctx, 0x30000) == 0xa5161514, "valid disc handle did not publish selected ISO bytes");
    check(fixture.Read(ctx, base, file, 8) == 0 && fixture.Get(ctx, 0x10804) == 4 && fixture.Get(ctx, 0x30000) == 0x1a191817, "partial EOF did not publish exact remaining bytes");
    const auto calls = fixture.calls.load();
    check(fixture.Read(ctx, base, file, 1) == 0xc0000011 && fixture.Get(ctx, 0x10804) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a5a5 && fixture.calls == calls, "EOF issued I/O or changed bytes");
    check(fixture.Read(ctx, base, file, 0) == 0 && fixture.Get(ctx, 0x10804) == 0 && fixture.calls == calls, "zero read issued I/O");
    fixture.Put(ctx, 0x10900, 1); fixture.Put(ctx, 0x10904, 0);
    check(fixture.Read(ctx, base, file, 2, 0x10900) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a51615, "explicit offset ignored");
    check(fixture.Read(ctx, base, file, 1) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a517, "explicit read did not advance synchronous cursor");
    fixture.Put(ctx, 0x10900, 0xfffffffe); fixture.Put(ctx, 0x10904, 0xffffffff);
    check(fixture.Read(ctx, base, file, 1, 0x10900) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a518, "FILE_USE_FILE_POINTER_POSITION rejected");
    fixture.Put(ctx, 0x10900, 0xffffffff);
    check(fixture.Read(ctx, base, file, 1, 0x10900) == 0xc000000d, "negative byte offset admitted");
    fixture.Put(ctx, 0x10900, 0); fixture.Put(ctx, 0x10904, 1);
    check(fixture.Read(ctx, base, file, 1, 0x10900) == 0xc0000011, "large positive offset wrapped");
    check(fixture.Read(ctx, base, file, 1, 0, 0x30000, 0x10800, 1) == 0xc00000bb, "event completion accepted");
    check(fixture.Read(ctx, base, file, 1, 0, 0x30000, 0x10800, 0, 1) == 0xc00000bb, "APC accepted");
    check(fixture.Read(ctx, base, file, 1, 0, 0x30000, 0x10800, 0, 0, 1) == 0xc00000bb, "APC context accepted");
    check(fixture.Read(ctx, base, file, (1u << 20) + 1) == 0xc000000d, "oversized read accepted");
    const auto async = fixture.Open(ctx, base, "D:\\story.dat", 0x100001, 0);
    fixture.Open(ctx, base, "D:\\story.dat", 0x100001, 0x10, 0xc00000bb);
    const auto noAccess = fixture.Open(ctx, base, "D:\\story.dat", 0x100000);
    const auto directory = fixture.Open(ctx, base, "D:\\", 0x100001, 0x21);
    check(fixture.Read(ctx, base, async, 1) == 0xc00000bb, "asynchronous mode accepted");
    check(fixture.Read(ctx, base, noAccess, 1) == 0xc0000022 && fixture.Read(ctx, base, directory, 1) == 0xc00000ba, "access or directory contract violated");
    for (uint32_t n = 0; n < 8; ++n) {
      const auto before = fixture.calls.load();
      bool refused = false;
      try {
        fixture.Read(ctx, base, file, 8, n == 2 ? 0x20000 : n == 3 ? 0x10800 : n == 4 ? 0x30000 : 0,
          n == 0 ? 0x20000 : n == 1 ? 0x10804 : n == 7 ? 0x130ffc : 0x30000, n == 5 ? 0x20000 : n == 6 ? 0x10801 : 0x10800);
      } catch (const browser::Fault&) { refused = true; }
      check(refused && fixture.calls == before && fixture.Get(ctx, 0x10800) == 0xabcdef && fixture.Get(ctx, 0x30000) == 0xa5a5a5a5, "invalid spans issued I/O or partially published");
    }
    for (uint32_t n = 0; n < 3; ++n) {
      const auto before = fixture.calls.load();
      const auto owner = ctx.host;
      const auto kpcr = ctx.fs_base;
      browser::Runtime other({}, {});
      bool refused = false;
      try {
        if (n == 0) { ctx.host = &other; fixture.Read(ctx, base, file, 1); }
        if (n == 1) { ctx.fs_base ^= 4; fixture.Read(ctx, base, file, 1); }
        if (n == 2) { auto lease = fixture.image.memory->Borrow(); fixture.Read(ctx, base, file, 1); }
      } catch (const browser::Fault&) { refused = true; }
      ctx.host = owner; ctx.fs_base = kpcr;
      check(refused && fixture.calls == before && fixture.Get(ctx, 0x10800) == 0xabcdef, "foreign owner or inherited lease issued read or published outputs");
    }
    const auto independent = fixture.Open(ctx, base);
    check(fixture.Read(ctx, base, independent, 1) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a514 && fixture.close(independent), "separate handles shared a file cursor");
    check(fixture.close(file) && fixture.Read(ctx, base, file, 1) == 0xc0000008, "closed handle remained readable");
    check(fixture.Read(ctx, base, 0x70000004, 1) == 0xc0000008, "foreign handle admitted");
    const auto executable = fixture.Open(ctx, base, "d:\\DeFaUlT.xBe");
    check(fixture.Read(ctx, base, executable, 20) == 0 && fixture.Get(ctx, 0x10804) == 7, "executable read failed");
    const auto counts = fixture.snapshot();
    check(counts.assetBytes == 12 && counts.assetOperations == 6 && counts.executableBytes == 7 && counts.executableOperations == 1, "successful guest delivery counters not exact or executable classification wrong");
  });
  Fixture isolated;
  check(isolated.snapshot().assetBytes == 0 && isolated.snapshot().executableBytes == 0, "disc read counters leaked across sessions");
}

void Failures() {
  for (uint32_t n = 0; n < 6; ++n) {
    Fixture fixture;
    fixture.Run([&](X86Context& ctx, uint8_t* base) {
      const auto file = fixture.Open(ctx, base);
      fixture.reader = [&](uint64_t at, void* out, uint32_t length) -> uint32_t {
        check(!browser::Memory::HasBorrow(), "disc I/O inherited lease");
        std::thread observer([&] {
          fixture.image.memory->Ensure(0x150000, 4096);
          if (n == 0) { check(fixture.close(file), "in-flight handle could not close"); }
          if (n == 1) { fixture.runtime.Cancel(); }
          if (n == 2) { fixture.image.memory->Decommit(0x30000, 4096); }
          if (n == 5) { fixture.image.memory->Decommit(0x10000, 4096); }
        });
        observer.join();
        if (n == 4) { throw std::runtime_error("invented backing error"); }
        std::memcpy(out, fixture.bytes->data() + at, length);
        return n == 3 ? length - 1 : length;
      };
      bool refused = false;
      uint32_t status = 0;
      try { status = fixture.Read(ctx, base, file, 3); }
      catch (const browser::Fault&) { refused = true; }
      if (n == 2) { fixture.image.memory->Commit(0x30000, 4096); }
      if (n == 5) { fixture.image.memory->Commit(0x10000, 4096); }
      check((n == 0 ? status == 0xc0000008 : n == 3 || n == 4 ? status == 0xc0000185 : refused), "in-flight failure not surfaced truthfully");
      check(fixture.Get(ctx, 0x30000) == (n == 2 ? 0u : 0xa5a5a5a5u), "failed/cancelled read published destination");
      const auto counts = fixture.snapshot();
      check(counts.assetBytes == 0 && counts.assetOperations == 0 && counts.executableBytes == 0 && counts.executableOperations == 0, "failed/cancelled reads counted as guest delivery");
      check(fixture.Get(ctx, 0x10800) == (n == 5 ? 0 : refused ? 0xabcdef : status) && fixture.Get(ctx, 0x10804) == (n == 5 ? 0 : refused ? 0xabcdef : 0), "failed read partially published status");
      if (n == 3 || n == 4) {
        fixture.reader = [&](uint64_t at, void* out, uint32_t length) {
          std::memcpy(out, fixture.bytes->data() + at, length);
          return length;
        };
        check(fixture.Read(ctx, base, file, 1) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a514, "failed read advanced cursor");
      }
    });
  }
}

void PositionCancellation() {
  Fixture fixture;
  fixture.Run([&](X86Context& ctx, uint8_t* base) {
    const auto file = fixture.Open(ctx, base);
    const std::array<uint32_t, 6> frame{0, file, 0x10800, 0x10900, 8, 14};
    fixture.image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    fixture.Put(ctx, 0x10800, 0xabcdef); fixture.Put(ctx, 0x10804, 0xabcdef);
    fixture.Put(ctx, 0x10900, 4); fixture.Put(ctx, 0x10904, 0);
    fixture.runtime.Cancel();
    bool cancelled = false;
    try { fixture.runtime.Kernel(ctx, base, 226); }
    catch (const browser::Cancelled&) { cancelled = true; }
    check(cancelled && fixture.Get(ctx, 0x10800) == 0xabcdef && fixture.Get(ctx, 0x10804) == 0xabcdef && fixture.calls == 0 &&
      fixture.snapshot().assetBytes == 0, "cancelled source position published outputs or performed I/O");
  });
}

void CacheReadCancellation() {
  Fixture fixture;
  browser::Runtime foreign({}, {});
  const std::array<uint8_t, 3> payload{1, 2, 3};
  check(fixture.temporary->WriteFile("DATA", payload) == 0, "cache cancellation fixture failed");
  fixture.Run([&](X86Context& ctx, uint8_t* base) {
    const auto file = fixture.Open(ctx, base, "\\Device\\Harddisk0\\Partition1\\DATA", 0x80100080, 0x60);
    const std::array<uint32_t, 9> frame{0, file, 0, 0, 0, 0x10800, 0x30000, 3, 0};
    fixture.image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    fixture.Put(ctx, 0x10800, 0xabcdef); fixture.Put(ctx, 0x10804, 0xabcdef); fixture.Put(ctx, 0x30000, 0xa5a5a5a5);
    auto wrong = ctx; wrong.host = &foreign;
    bool refused = false;
    try { fixture.runtime.Kernel(wrong, base, 219); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && fixture.Get(ctx, 0x10800) == 0xabcdef && fixture.Get(ctx, 0x30000) == 0xa5a5a5a5,
      "foreign session read cache backing or published guest bytes");
    check(fixture.Read(ctx, base, file, 1) == 0 && fixture.Get(ctx, 0x30000) == 0xa5a5a501,
      "foreign cache read advanced owned handle cursor");
    fixture.image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    fixture.Put(ctx, 0x10800, 0xabcdef); fixture.Put(ctx, 0x10804, 0xabcdef); fixture.Put(ctx, 0x30000, 0xa5a5a5a5);
    const auto usage = fixture.temporary->Usage();
    const auto metadata = fixture.temporary->Stat("DATA");
    fixture.runtime.Cancel();
    bool cancelled = false;
    try { fixture.runtime.Kernel(ctx, base, 219); }
    catch (const browser::Cancelled&) { cancelled = true; }
    std::array<uint8_t, 3> bytes{};
    check(cancelled && fixture.Get(ctx, 0x10800) == 0xabcdef && fixture.Get(ctx, 0x10804) == 0xabcdef && fixture.Get(ctx, 0x30000) == 0xa5a5a5a5 &&
      fixture.temporary->Read("DATA", 0, bytes) == bytes.size() && bytes == payload && fixture.temporary->Usage().allocated == usage.allocated &&
      fixture.temporary->Usage().entries == usage.entries && fixture.temporary->Stat("DATA")->lastWriteTime == metadata->lastWriteTime &&
      fixture.snapshot().assetBytes == 0 && fixture.snapshot().assetOperations == 0 && fixture.snapshot().executableBytes == 0 && fixture.calls == 0,
      "cancelled cache219 changed backing, quota, outputs or source-only delivery counters");
  });
}

void ConcurrentPosition(bool cache) {
  Fixture fixture;
  check(fixture.temporary->WriteFile("CACHE.DAT", std::array<uint8_t, 7>{20, 21, 22, 23, 24, 25, 26}) == 0, "cache concurrent fixture failed");
  const auto usage = fixture.temporary->Usage();
  const auto metadata = fixture.temporary->Stat("CACHE.DAT");
  uint32_t handle = 0;
  fixture.Run([&](X86Context& ctx, uint8_t* base) {
    handle = fixture.Open(ctx, base, cache ? "\\Device\\Harddisk0\\Partition1\\CACHE.DAT" : "D:\\story.dat", 0x80100080, 0x60);
  });
  std::atomic<uint32_t> ready = 0, completed = 0;
  for (uint32_t n = 0; n < 2; ++n) {
    X86Context context{}; context.host = &fixture.runtime; context.fs_base = 0x18000 + n * 4096; context.esp = 0x13000 + n * 0x4000;
    fixture.runtime.Launch(context, fixture.image.memory->Base(), [&, n](X86Context& ctx, uint8_t* base) {
      const uint32_t arena = 0x11000 + n * 0x4000;
      fixture.Put(ctx, arena + 32, 0); fixture.Put(ctx, arena + 36, 0);
      ++ready;
      while (ready != 2) { fixture.runtime.Checkpoint(ctx); std::this_thread::yield(); }
      for (uint32_t count = 0; count < 64; ++count) {
        const auto stack = ctx.esp;
        check(fixture.runtime.Call(ctx, base, fixture.runtime.KernelTarget(226), {handle, arena, arena + 32, 8, 14}) == 0 &&
          fixture.Get(ctx, arena) == 0 && fixture.Get(ctx, arena + 4) == 0 && ctx.esp == stack, "threaded source position failed");
        check(fixture.runtime.Call(ctx, base, fixture.runtime.KernelTarget(219), {handle, 0, 0, 0, arena, arena + 16, 1, 0}) == 0 &&
          fixture.Get(ctx, arena + 4) == 1 && ctx.esp == stack, "threaded source position/read advanced beyond serialized limit");
        uint8_t byte = 0;
        fixture.image.memory->Read(ctx, arena + 16, &byte, 1);
        check(byte == 20 || byte == 21, "concurrent seek/read observed torn or stale cursor");
        ++completed;
      }
    });
  }
  if (const auto failure = fixture.runtime.Join()) { std::rethrow_exception(failure); }
  check(completed == 128 && fixture.snapshot().assetBytes == (cache ? 0u : 128u) && fixture.snapshot().assetOperations == (cache ? 0u : 128u) &&
    fixture.snapshot().executableBytes == 0 && fixture.temporary->Usage().allocated == usage.allocated && fixture.temporary->Usage().entries == usage.entries &&
    fixture.temporary->Stat("CACHE.DAT")->lastWriteTime == metadata->lastWriteTime && fixture.temporary->Stat("CACHE.DAT")->size == metadata->size &&
    fixture.close(handle), "threaded owned seek/read corrupted retained bytes, quota, source-only counters or handle lifecycle");
}

void Concurrent() {
  Fixture fixture;
  uint32_t handle = 0;
  fixture.Run([&](X86Context& ctx, uint8_t* base) { handle = fixture.Open(ctx, base); });
  std::atomic<uint32_t> mask = 0, completed = 0;
  std::atomic<bool> done = false, torn = false;
  std::thread observer([&] {
    while (!done.load()) {
      const auto counts = fixture.snapshot();
      if (counts.assetBytes != counts.assetOperations || counts.executableBytes || counts.executableOperations) { torn = true; }
      std::this_thread::yield();
    }
  });
  for (uint32_t n = 0; n < 2; ++n) {
    X86Context context{}; context.host = &fixture.runtime; context.fs_base = 0x18000 + n * 4096; context.esp = 0x13000 + n * 0x4000;
    fixture.runtime.Launch(context, fixture.image.memory->Base(), [&, n](X86Context& ctx, uint8_t* base) {
      const uint32_t arena = 0x11000 + n * 0x4000;
      for (uint32_t count = 0; count < 16; ++count) {
        const auto result = fixture.runtime.Call(ctx, base, fixture.runtime.KernelTarget(219), {handle, 0, 0, 0, arena, arena + 16, 1, 0});
        if (!result) {
          uint8_t byte = 0;
          fixture.image.memory->Read(ctx, arena + 16, &byte, 1);
          check(byte >= 20 && byte <= 26, "concurrent read returned wrong source byte");
          mask.fetch_or(1u << (byte - 20));
          ++completed;
        } else { check(result == 0xc0000011, "serialized cursor returned unsupported status"); }
      }
    });
  }
  const auto failure = fixture.runtime.Join();
  done = true;
  observer.join();
  if (failure) { std::rethrow_exception(failure); }
  const auto counts = fixture.snapshot();
  check(!torn && completed == 7 && mask == 127 && counts.assetBytes == 7 && counts.assetOperations == 7, "concurrent cursor or coherent snapshot failed");
}
}

int main() {
  UnbufferedAdvisory();
  PositionAndMetadata();
  PositionCancellation();
  CacheReadCancellation();
  ConcurrentPosition(false);
  ConcurrentPosition(true);
  Contracts();
  Failures();
  Concurrent();
  std::puts("disc-file-read-contracts-ok");
}
