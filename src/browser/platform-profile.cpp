#include "browser/platform-profile.h"
#include "browser/kernel-provider.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(int, virtualDriveEntropy, (uint8_t* destination), {
  const crypto = globalThis.crypto;
  if (!crypto || typeof crypto.getRandomValues !== 'function') { return 0; }
  const bytes = new Uint8Array(16);
  try {
    crypto.getRandomValues(bytes);
    HEAPU8.set(bytes, destination);
    return 1;
  } catch {
    return 0;
  } finally {
    bytes.fill(0);
  }
});
#else
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>
#endif
#endif

namespace browser {

KernelDataExport InstallVirtualDriveKey(Runtime& runtime, PreparedImage& image) {
  auto* const memory = image.memory.get();
  const auto preflight = [&] {
    runtime.Checkpoint({});
    if (!memory || image.memory.get() != memory || image.importsBound || image.boundRuntime ||
        image.diagnosticRuntime || !image.diagnosticImports.empty() || runtime.ActiveWorkers() ||
        runtime.HasPublishedImage() || runtime.KernelTarget(323) || Memory::HasBorrow()) {
      throw Fault({}, 323, 16, "virtual drive key requires an unclaimed unpublished stopped image");
    }
  };
  preflight();
  constexpr uint32_t address = 0xc0003000;
  auto mutation = memory->Mutate();
  if (memory->QueryPages(address).mapped) {
    throw Fault({}, address, 16, "virtual drive key requires fresh owned storage");
  }
  struct Entropy {
    std::array<uint8_t, 16> bytes{};
    ~Entropy() {
      volatile uint8_t* destination = bytes.data();
      for (size_t n = 0; n < bytes.size(); ++n) { destination[n] = 0; }
    }
  } entropy;
#ifdef __EMSCRIPTEN__
  const bool generated = virtualDriveEntropy(entropy.bytes.data()) != 0;
#else
  const bool generated = getentropy(entropy.bytes.data(), entropy.bytes.size()) == 0;
#endif
  if (!generated) { throw Fault({}, 323, 16, "secure virtual drive key entropy unavailable"); }
  preflight();
  memory->Map(address, 4096);
  struct PendingMapping {
    Memory& memory;
    uint32_t address;
    bool installed = false;
    ~PendingMapping() { if (!installed) { memory.Discard(address, 4096); } }
  } pending{*memory, address};
  memory->Commit(address, 4096);
  memory->Write({}, address, entropy.bytes.data(), entropy.bytes.size());
  preflight();
  image.diagnosticRuntime = &runtime;
  pending.installed = true;
  return {323, address, 16};
}

KernelDataExport InstallSignatureKey(Runtime& runtime, PreparedImage& image) {
  auto* const memory = image.memory.get();
  const auto preflight = [&] {
    runtime.Checkpoint({});
    if (!memory || image.memory.get() != memory || image.importsBound || image.boundRuntime ||
        (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) || !image.diagnosticImports.empty() ||
        runtime.ActiveWorkers() || runtime.HasPublishedImage() || runtime.KernelTarget(325) || Memory::HasBorrow()) {
      throw Fault({}, 325, 16, "signature key requires an unclaimed unpublished stopped image");
    }
  };
  preflight();
  constexpr uint32_t address = 0xc0004000;
  auto mutation = memory->Mutate();
  if (memory->QueryPages(address).mapped) {
    throw Fault({}, address, 16, "signature key requires fresh owned storage");
  }
  preflight();
  memory->Map(address, 4096);
  struct PendingMapping {
    Memory& memory;
    bool installed = false;
    ~PendingMapping() { if (!installed) { memory.Discard(0xc0004000, 4096); } }
  } pending{*memory};
  memory->Commit(address, 4096);
  constexpr std::array<uint8_t, 16> initial{};
  memory->Write({}, address, initial.data(), initial.size());
  preflight();
  image.diagnosticRuntime = &runtime;
  pending.installed = true;
  return {325, address, 16};
}

std::array<KernelDataExport, 6> InstallPlatformProfile(Runtime& runtime, PreparedImage& image) {
  auto* const memory = image.memory.get();
  const auto preflight = [&] {
    runtime.Checkpoint({});
    if (!memory || image.memory.get() != memory || image.importsBound || image.boundRuntime ||
        (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) || !image.diagnosticImports.empty() ||
        runtime.ActiveWorkers() || runtime.HasPublishedImage() || runtime.KernelTarget(41) || runtime.KernelTarget(42) ||
        runtime.KernelTarget(322) || runtime.KernelTarget(324) || runtime.KernelTarget(164) || runtime.KernelTarget(356) || Memory::HasBorrow()) {
      throw Fault({}, 41, 8, "platform profile requires an unclaimed unpublished stopped image");
    }
  };
  preflight();
  constexpr uint32_t address = 0xc0000000;
  constexpr std::array<uint8_t, 20> profile = {0x20, 0, 0, 0, 0xd3, 0xb1, 0, 0, 1, 0, 0, 0, 0xce, 0x16, 1, 0, 0, 0, 0, 0};
  constexpr std::array<uint8_t, 8> descriptor = {21, 0, 22, 0, 0x1c, 0, 0, 0xc0};
  constexpr char model[] = "Browser Virtual Cache";
  auto mutation = memory->Mutate();
  if (memory->QueryPages(address).mapped) {
    throw Fault({}, address, 4096, "platform profile requires fresh owned storage");
  }
  struct Entropy {
    std::array<uint8_t, 16> bytes{};
    ~Entropy() {
      volatile uint8_t* destination = bytes.data();
      for (size_t n = 0; n < bytes.size(); ++n) { destination[n] = 0; }
    }
  } entropy;
#ifdef __EMSCRIPTEN__
  const bool generated = virtualDriveEntropy(entropy.bytes.data()) != 0;
#else
  const bool generated = getentropy(entropy.bytes.data(), entropy.bytes.size()) == 0;
#endif
  if (!generated) { throw Fault({}, 42, 8, "secure virtual serial entropy unavailable"); }
  constexpr char hex[] = "0123456789ABCDEF";
  std::array<char, 21> serial{};
  for (size_t n = 0; n < 10; ++n) {
    serial[2 * n] = hex[entropy.bytes[n] >> 4];
    serial[2 * n + 1] = hex[entropy.bytes[n] & 15];
  }
  constexpr std::array<uint8_t, 8> serialDescriptor = {20, 0, 21, 0, 0x3c, 0, 0, 0xc0};
  preflight();
  memory->Map(address, 4096);
  struct PendingMapping {
    Memory& memory;
    bool installed = false;
    ~PendingMapping() { if (!installed) { memory.Discard(0xc0000000, 4096); } }
  } pending{*memory};
  memory->Commit(address, 4096);
  memory->Write({}, address, profile.data(), profile.size());
  memory->Write({}, address + 20, descriptor.data(), descriptor.size());
  memory->Write({}, address + 28, model, sizeof(model));
  memory->Write({}, address + 52, serialDescriptor.data(), serialDescriptor.size());
  memory->Write({}, address + 60, serial.data(), serial.size());
  memory->Write({}, address + 84, &KernelVideoPack, sizeof(KernelVideoPack));
  preflight();
  image.diagnosticRuntime = &runtime;
  pending.installed = true;
  return {{{322, address, 8}, {324, address + 8, 8}, {164, address + 16, 4}, {41, address + 20, 8}, {42, address + 52, 8}, {356, address + 84, 4}}};
}

}
