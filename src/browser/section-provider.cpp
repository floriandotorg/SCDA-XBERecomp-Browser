#include "browser/section-provider.h"
#include <algorithm>
#include <array>

namespace browser {
namespace {

class SectionProvider {
  struct Counter {
    uint32_t address;
    uint32_t page;
    uint16_t value = 0;
  };
  Runtime& runtime_;
  PreparedImage& image_;
  Memory& memory_;
  std::shared_ptr<const xbe::Image> sourceOwner_;
  const xbe::Image& source_;
  uint32_t table_ = 0;
  std::vector<uint32_t> references_;
  std::vector<uint32_t> pageOwners_;
  std::vector<Counter> counters_;

  uint32_t Field(size_t section, uint32_t offset) const {
    return source_.U32(table_ - source_.base + uint32_t(section) * 56 + offset);
  }

  Counter& CounterAt(uint32_t address) {
    const auto found = std::find_if(counters_.begin(), counters_.end(), [&](const auto& counter) {
      return counter.address == address;
    });
    if (found == counters_.end()) { throw Fault({}, address, 2, "unowned section page counter"); }
    return *found;
  }

  void Validate(const X86Context& context, bool initial) {
    for (size_t n = 0; n < references_.size(); ++n) {
      std::array<uint32_t, 14> header;
      memory_.Read(context, table_ + uint32_t(n) * 56, header.data(), sizeof(header));
      for (uint32_t field = 0; field < header.size(); ++field) {
        const uint32_t expected = field == 6 ? references_[n] : Field(n, field * 4);
        if (header[field] != expected) {
          throw Fault(context, table_ + uint32_t(n) * 56, 56, "owned section header was modified");
        }
      }
    }
    for (const auto& counter : counters_) {
      uint16_t value = 0;
      memory_.Read(context, counter.address, &value, 2);
      if (value != (initial ? 0 : counter.value)) {
        throw Fault(context, counter.address, 2, "owned section page counter was modified");
      }
    }
    for (size_t n = 0; n < pageOwners_.size(); ++n) {
      const uint32_t address = source_.base + uint32_t(n) * 4096;
      if (memory_.IsCommitted(address) != bool(pageOwners_[n])) {
        throw Fault(context, address, 4096, "owned image page commitment was modified");
      }
    }
  }

public:
  SectionProvider(Runtime& runtime, PreparedImage& image) : runtime_(runtime), image_(image), memory_(*image.memory),
      sourceOwner_(image.AuthenticatedSource()), source_(*sourceOwner_) {
    table_ = source_.U32(0x120);
    references_.resize(source_.sections.size());
    pageOwners_.resize((source_.image_size + 4095) / 4096);
    for (uint32_t n = 0; n < (source_.header_size + 4095) / 4096; ++n) { pageOwners_[n] = 1; }
    for (size_t n = 0; n < source_.sections.size(); ++n) {
      const auto& section = source_.sections[n];
      if (Field(n, 24)) { throw Fault({}, table_ + uint32_t(n) * 56, 56, "authenticated section has a nonzero initial reference count"); }
      const uint32_t first = (section.va - source_.base) / 4096;
      const uint32_t last = (section.va - source_.base + section.vsize - 1) / 4096;
      for (const auto& [offset, page] : std::array<std::pair<uint32_t, uint32_t>, 2>{{{28, first}, {32, last}}}) {
        const uint32_t address = Field(n, offset);
        const uint64_t end = uint64_t(address) + 2;
        const auto overlaps = [&](uint32_t start, uint64_t size) { return address < uint64_t(start) + size && end > start; };
        if (address % 2 || address < uint64_t(source_.base) + 0x180 || end > uint64_t(source_.base) + source_.header_size ||
            overlaps(table_, source_.sections.size() * 56) || overlaps(source_.cert, source_.U32(source_.cert - source_.base))) {
          throw Fault({}, address, 2, "invalid authenticated section page counter extent");
        }
        for (size_t name = 0; name < source_.sections.size(); ++name) {
          if (overlaps(Field(name, 20), source_.sections[name].name.size() + 1)) {
            throw Fault({}, address, 2, "section page counter aliases a section name");
          }
        }
        const auto found = std::find_if(counters_.begin(), counters_.end(), [&](const auto& counter) {
          return counter.address == address || counter.page == page;
        });
        if (found == counters_.end()) {
          if (source_.U32(address - source_.base) & 0xffff) { throw Fault({}, address, 2, "authenticated section page counter is not zero"); }
          counters_.push_back({address, page});
        } else if (found->address != address || found->page != page) {
          throw Fault({}, address, 2, "section page counter does not identify its shared page");
        }
      }
      if (section.flags & xbe::kPreload) {
        references_[n] = 1;
        for (uint32_t page = first; page <= last; ++page) { ++pageOwners_[page]; }
        ++CounterAt(Field(n, 28)).value;
        ++CounterAt(Field(n, 32)).value;
      }
    }
    auto mutation = memory_.Mutate();
    auto lease = memory_.Borrow();
    Validate({}, true);
    runtime_.Checkpoint({});
  }

  void Initialize() {
    auto mutation = memory_.Mutate();
    auto lease = memory_.Borrow();
    for (const auto& counter : counters_) { memory_.Write({}, counter.address, &counter.value, 2); }
  }

  void Invoke(X86Context& context, uint8_t* base, bool unload) {
    const uint32_t ordinal = unload ? 328 : 327;
    runtime_.Checkpoint(context);
    if (!runtime_.OwnsCurrentContext(context) || base != memory_.Base() || image_.memory.get() != &memory_ ||
        !image_.importsBound || image_.boundRuntime != &runtime_) {
      throw Fault(context, ordinal, 0, "section operation requires its current owned bound image context");
    }
    if (Memory::HasBorrow()) { throw Fault(context, ordinal, 0, "section operation during inherited backing lease"); }
    const uint64_t stackEnd = uint64_t(context.esp) + 8;
    if (stackEnd > UINT32_MAX) { throw Fault(context, context.esp, 8, "section operation frame overflow"); }
    auto mutation = memory_.Mutate();
    size_t selected = 0;
    {
      auto lease = memory_.Borrow();
      std::array<uint32_t, 2> frame;
      memory_.Read(context, context.esp, frame.data(), sizeof(frame));
      if (frame[1] < table_ || (frame[1] - table_) % 56 || (frame[1] - table_) / 56 >= references_.size()) {
        throw Fault(context, frame[1], 56, "section operation requires exact owned header identity");
      }
      selected = (frame[1] - table_) / 56;
      Validate(context, false);
      const auto& section = source_.sections[selected];
      const auto& head = CounterAt(Field(selected, 28));
      const auto& tail = CounterAt(Field(selected, 32));
      const uint32_t endpointUses = head.address == tail.address ? 2 : 1;
      if (unload) {
        if (!references_[selected]) { throw Fault(context, frame[1], 56, "section unload requires a live owned reference"); }
        if (references_[selected] == 1) {
          if (head.value < endpointUses || tail.value < endpointUses) {
            throw Fault(context, frame[1], 56, "section page counter underflow");
          }
          const uint32_t first = (section.va - source_.base) / 4096;
          const uint32_t last = (section.va - source_.base + section.vsize - 1) / 4096;
          for (uint32_t page = first; page <= last; ++page) {
            const uint32_t address = source_.base + page * 4096;
            if (!pageOwners_[page]) { throw Fault(context, address, 4096, "section page ownership underflow"); }
            if (pageOwners_[page] == 1 && address < stackEnd && uint64_t(address) + 4096 > context.esp) {
              throw Fault(context, address, 4096, "section unload decommit aliases its call frame");
            }
          }
        }
      } else {
        if (references_[selected] == UINT32_MAX) { throw Fault(context, frame[1], 56, "section reference count overflow"); }
        if (!references_[selected]) {
          if (uint32_t(head.value) + endpointUses > UINT16_MAX || uint32_t(tail.value) + endpointUses > UINT16_MAX) {
            throw Fault(context, frame[1], 56, "section page counter overflow");
          }
          if (section.va < stackEnd && uint64_t(section.va) + section.vsize > context.esp) {
            throw Fault(context, section.va, section.vsize, "section load destination aliases its call frame");
          }
        }
      }
      runtime_.Checkpoint(context);
    }
    const auto& section = source_.sections[selected];
    if (unload) {
      if (references_[selected] == 1) {
        const uint32_t first = (section.va - source_.base) / 4096;
        const uint32_t last = (section.va - source_.base + section.vsize - 1) / 4096;
        for (uint32_t page = first; page <= last; ++page) {
          if (!--pageOwners_[page]) {
            memory_.Decommit(source_.base + page * 4096, 4096);
          }
        }
        --CounterAt(Field(selected, 28)).value;
        --CounterAt(Field(selected, 32)).value;
        for (const auto& counter : counters_) { memory_.Write(context, counter.address, &counter.value, 2); }
      }
      --references_[selected];
    } else {
      Load(context, selected);
    }
    memory_.Write(context, table_ + uint32_t(selected) * 56 + 24, &references_[selected], 4);
    context.eax = 0;
    context.esp = uint32_t(stackEnd);
  }

  void Load(const X86Context& context, size_t selected) {
    const auto& section = source_.sections[selected];
    if (!references_[selected]) {
      memory_.Commit(section.va, section.vsize);
      auto lease = memory_.Borrow();
      const std::array<uint8_t, 4096> zeros{};
      memory_.Write(context, section.va, source_.file.data() + section.raw, section.rsize);
      for (uint32_t offset = section.rsize; offset < section.vsize;) {
        const uint32_t count = std::min(uint32_t(zeros.size()), section.vsize - offset);
        memory_.Write(context, section.va + offset, zeros.data(), count);
        offset += count;
      }
      const uint32_t first = (section.va - source_.base) / 4096;
      const uint32_t last = (section.va - source_.base + section.vsize - 1) / 4096;
      for (uint32_t page = first; page <= last; ++page) { ++pageOwners_[page]; }
      ++CounterAt(Field(selected, 28)).value;
      ++CounterAt(Field(selected, 32)).value;
      for (const auto& counter : counters_) { memory_.Write(context, counter.address, &counter.value, 2); }
    }
    ++references_[selected];
  }
};

}

void InstallSectionProvider(Runtime& runtime, PreparedImage& image) {
  runtime.Checkpoint({});
  if (!image.memory || !image.AuthenticatedSource() || image.importsBound || runtime.ActiveWorkers() || Memory::HasBorrow() || runtime.KernelTarget(327) || runtime.KernelTarget(328) ||
      (image.diagnosticRuntime && image.diagnosticRuntime != &runtime)) {
    throw Fault({}, 327, 0, "section installation requires its unpublished stopped prepared image");
  }
  auto provider = std::make_shared<SectionProvider>(runtime, image);
  runtime.RegisterKernel(327, [provider](X86Context& context, uint8_t* base) { provider->Invoke(context, base, false); });
  runtime.RegisterKernel(328, [provider](X86Context& context, uint8_t* base) { provider->Invoke(context, base, true); });
  provider->Initialize();
}

}
