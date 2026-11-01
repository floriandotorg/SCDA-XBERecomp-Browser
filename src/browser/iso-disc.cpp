#include "browser/iso-disc.h"
#include <algorithm>

namespace browser {

uint32_t ReadDiscToGuest(const kernel::DiscIndex& disc, const kernel::DiscIndex::Entry& file,
  uint64_t position, Memory& memory, Runtime& runtime, const X86Context& context,
  uint32_t destination, uint32_t length) {
  runtime.Checkpoint(context);
  if (Memory::HasBorrow() || length > (1u << 20)) {
    throw Fault(context, destination, length, "invalid guest ISO read budget or backing lease");
  }
  if (file.dir || position >= file.size || length == 0) { return 0; }
  const auto count = uint32_t(std::min<uint64_t>(length, file.size - position));
  memory.CheckRange(context, destination, count);
  std::vector<uint8_t> staging(count);
  const uint32_t completed = disc.Read(file, position, staging.data(), count);
  runtime.Checkpoint(context);
  if (completed != count) { return 0; }
  memory.Write(context, destination, staging.data(), count);
  return count;
}

}
