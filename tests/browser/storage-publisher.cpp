#include "browser/storage-publisher.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

using browser::ConsoleIdentity;
using browser::StoragePublisher;
using browser::TemporaryFilesystem;
using namespace std::chrono_literals;

const ConsoleIdentity identity = [] {
  ConsoleIdentity value;
  for (uint8_t n = 0; n < value.driveKey.size(); ++n) { value.driveKey[n] = uint8_t(n * 17 + 3); }
  std::memcpy(value.serial.data(), "0123456789ABCDEF0A1B", value.serial.size());
  return value;
}();

const auto images = [] {
  TemporaryFilesystem files(64 * TemporaryFilesystem::AllocationUnit);
  const std::array<uint8_t, 4> bytes{9, 8, 7, 6};
  check(files.CreateDirectory("UDATA") == 0 && files.WriteFile("UDATA\\SAVE", bytes) == 0, "fixture failed");
  uint64_t revision = 0;
  const auto image = browser::EncodeStorageImage(identity, files, &revision);
  check(revision == files.Revision(), "encoding did not report the revision it captured");
  TemporaryFilesystem restored(64 * TemporaryFilesystem::AllocationUnit);
  ConsoleIdentity decoded{};
  check(browser::DecodeStorageImage(image, decoded, restored) == 0, "a valid image did not decode");
  check(decoded.driveKey == identity.driveKey && decoded.serial == identity.serial, "console identity changed");
  check(restored.Serialize() == files.Serialize(), "files changed through the storage image");
  const auto rejects = [&](std::vector<uint8_t> damaged, const char* reason) {
    TemporaryFilesystem target(64 * TemporaryFilesystem::AllocationUnit);
    check(target.WriteFile("KEEP", bytes) == 0, "rejection fixture failed");
    const auto keep = target.Serialize();
    ConsoleIdentity untouched{};
    untouched.driveKey.fill(0x5a);
    const auto before = untouched;
    check(browser::DecodeStorageImage(damaged, untouched, target) != 0, reason);
    check(untouched.driveKey == before.driveKey && untouched.serial == before.serial && target.Serialize() == keep,
      "a rejected image changed the identity or the files");
  };
  rejects({}, "an empty image decoded");
  rejects(std::vector<uint8_t>(image.begin(), image.begin() + 30), "a truncated header decoded");
  auto magic = image;
  magic[1] ^= 0x20;
  rejects(magic, "a wrong magic decoded");
  auto version = image;
  version[8] = 9;
  rejects(version, "an unknown version decoded");
  auto serial = image;
  serial[12 + 16] = 'g';
  rejects(serial, "a serial with a non hex digit decoded");
  auto files_ = image;
  files_.back() ^= 0xff;
  files_.push_back(1);
  rejects(files_, "damaged files decoded");
  std::puts("storage-publisher-images-ok");
};

struct Recorder {
  std::mutex lock;
  std::condition_variable changed;
  std::vector<uint64_t> published;
  std::vector<bool> pending;
  std::vector<uint8_t> last;
  StoragePublisher::Sink sink() {
    return {
      [this](uint64_t revision, std::vector<uint8_t> image) {
        std::lock_guard guard(lock);
        published.push_back(revision);
        last = std::move(image);
        changed.notify_all();
      },
      [this](bool value) {
        std::lock_guard guard(lock);
        pending.push_back(value);
        changed.notify_all();
      },
    };
  }
};

const auto polling = [] {
  auto files = std::make_shared<TemporaryFilesystem>(64 * TemporaryFilesystem::AllocationUnit);
  Recorder recorder;
  StoragePublisher publisher(files, [&](uint64_t* revision) { return browser::EncodeStorageImage(identity, *files, revision); },
    recorder.sink(), {.settle = 500ms, .poll = 0ms});
  const auto start = StoragePublisher::Clock::time_point{} + 10s;
  publisher.Poll(start);
  check(recorder.published.empty() && recorder.pending.empty(), "an unchanged namespace published");
  const std::array<uint8_t, 3> bytes{1, 2, 3};
  check(files->WriteFile("PROFILE", bytes) == 0, "write failed");
  publisher.Poll(start + 1ms);
  check(recorder.pending == std::vector<bool>{true} && recorder.published.empty(), "a change was not reported as pending at once");
  publisher.Poll(start + 400ms);
  check(recorder.published.empty(), "a change published before it settled");
  check(files->Write("PROFILE", 3, bytes) == 0, "second write failed");
  publisher.Poll(start + 600ms);
  check(recorder.published.empty(), "a later change did not restart the settle time");
  {
    const auto writer = files->HoldWriter();
    publisher.Poll(start + 2s);
    check(recorder.published.empty(), "a namespace with an open writer published");
  }
  publisher.Poll(start + 2001ms);
  check(recorder.published == std::vector<uint64_t>{files->Revision()}, "a settled change did not publish once");
  check(recorder.pending == std::vector<bool>{true, false}, "publishing did not clear the pending state");
  ConsoleIdentity decoded{};
  TemporaryFilesystem restored(64 * TemporaryFilesystem::AllocationUnit);
  check(browser::DecodeStorageImage(recorder.last, decoded, restored) == 0 && restored.Serialize() == files->Serialize(),
    "the published image is not the settled namespace");
  publisher.Poll(start + 5s);
  check(recorder.published.size() == 1 && recorder.pending.size() == 2, "an unchanged namespace published again");
  check(files->Delete("PROFILE") == 0, "delete failed");
  publisher.Poll(start + 6s);
  publisher.Poll(start + 7s);
  check(recorder.published.size() == 2 && recorder.published.back() == files->Revision(), "a deletion did not publish");

  auto restoredFiles = std::make_shared<TemporaryFilesystem>(64 * TemporaryFilesystem::AllocationUnit);
  check(restoredFiles->Restore(files->Serialize()) == 0, "restore failed");
  Recorder quiet;
  StoragePublisher baseline(restoredFiles, [&](uint64_t* revision) { return browser::EncodeStorageImage(identity, *restoredFiles, revision); },
    quiet.sink(), {.settle = 500ms, .poll = 0ms});
  baseline.Poll(start);
  baseline.Poll(start + 10s);
  check(quiet.published.empty() && quiet.pending.empty(), "a restored namespace published before the guest changed it");
  std::puts("storage-publisher-polling-ok");
};

const auto threaded = [] {
  auto files = std::make_shared<TemporaryFilesystem>(64 * TemporaryFilesystem::AllocationUnit);
  Recorder recorder;
  {
    StoragePublisher publisher(files, [&](uint64_t* revision) { return browser::EncodeStorageImage(identity, *files, revision); },
      recorder.sink(), {.settle = 20ms, .poll = 5ms});
    const std::array<uint8_t, 2> bytes{4, 5};
    check(files->WriteFile("SAVE", bytes) == 0, "write failed");
    std::unique_lock guard(recorder.lock);
    check(recorder.changed.wait_for(guard, 5s, [&] { return !recorder.published.empty(); }), "the publisher thread did not publish");
    check(recorder.published.back() == files->Revision(), "the publisher thread published a stale revision");
  }
  const auto count = recorder.published.size();
  const std::array<uint8_t, 2> bytes{6, 7};
  check(files->WriteFile("LATER", bytes) == 0, "write after stop failed");
  std::this_thread::sleep_for(60ms);
  check(recorder.published.size() == count, "a stopped publisher kept publishing");
  std::puts("storage-publisher-threaded-ok");
};

int main(int argc, char** argv) {
  try {
    const std::string mode = argc == 2 ? argv[1] : "";
    if (mode == "images") { images(); }
    else if (mode == "polling") { polling(); }
    else if (mode == "threaded") { threaded(); }
    else { throw std::runtime_error("unknown mode"); }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "storage-publisher-failed: %s\n", error.what());
  }
  return 1;
}
