#include "browser/cache-fatx.h"
#include "browser/file-provider.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

std::atomic<bool> installing{false};
std::atomic<size_t> partitionAllocations{0};

void* operator new(size_t size) {
  if (installing && size == browser::CacheFatx::Capacity) { ++partitionAllocations; }
  auto* allocation = std::malloc(std::max<size_t>(size, 1));
  if (!allocation) { throw std::bad_alloc(); }
  return allocation;
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, size_t) noexcept { ::operator delete(pointer); }

int main() {
  try {
    browser::Runtime first{{}, {}};
    browser::Runtime second{{}, {}};
    const std::array runtimes{&first, &second};
    std::array<browser::PreparedImage, 2> images;
    std::array<std::function<bool(uint32_t)>, 2> providers;
    for (auto& image : images) { image.memory = std::make_unique<browser::Memory>(); }
    installing = true;
    for (size_t n = 0; n < images.size(); ++n) {
      providers[n] = browser::InstallFileProvider(*runtimes[n], images[n], {});
    }
    installing = false;
    if (partitionAllocations != 0) { throw std::runtime_error("disc-only provider installation allocated 64 MiB cache backing"); }
    for (size_t n = 0; n < providers.size(); ++n) {
      if (!providers[n] || !runtimes[n]->KernelTarget(202) || providers[n](0x12345678)) {
        throw std::runtime_error("disc-only provider installation changed public handle contracts");
      }
    }
    std::puts("cache-lazy-provider-contracts-ok: concurrent-sessions=2 install-partition-allocations=0");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
