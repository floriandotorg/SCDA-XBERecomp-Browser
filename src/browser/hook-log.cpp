#include "core/log.h"
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace logx {
namespace {
constexpr size_t kLine = 512;
constexpr size_t kRemembered = 256;
constexpr uint32_t kRepeatsShown = 4;
std::mutex lock;
std::unordered_map<std::string, uint32_t> repeats;
const std::string path = "stderr";

bool Shown(uint32_t count) { return count <= kRepeatsShown || (count & (count - 1)) == 0; }
}

void Init(const std::string&, const std::string&, bool) {}

void Write(Level level, const char* format, ...) {
  if (level == Level::Debug && !kVerbose) { return; }
  char line[kLine];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  std::lock_guard guard(lock);
  if (repeats.size() >= kRemembered && !repeats.contains(line)) { repeats.clear(); }
  const uint32_t count = ++repeats[line];
  if (!Shown(count)) { return; }
  if (count > kRepeatsShown) { std::fprintf(stderr, "[game] %s (x%u)\n", line, count); }
  else { std::fprintf(stderr, "[game] %s\n", line); }
}

void Flush() { std::fflush(stderr); }

const std::string& Path() { return path; }

}
