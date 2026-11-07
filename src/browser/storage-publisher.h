#pragma once

#include "browser/temporary-filesystem.h"
#include <chrono>
#include <condition_variable>
#include <thread>

namespace browser {

// the virtual hard disk key and serial the game signs its saves with: they travel with the saved files
struct ConsoleIdentity {
  std::array<uint8_t, 16> driveKey{};
  std::array<uint8_t, 20> serial{};
};

std::vector<uint8_t> EncodeStorageImage(const ConsoleIdentity& identity, const TemporaryFilesystem& files, uint64_t* revision = nullptr);
uint32_t DecodeStorageImage(std::span<const uint8_t> image, ConsoleIdentity& identity, TemporaryFilesystem& files);

// Publishes the writable namespace once the guest has left it alone: no open writer and no change for the settle time.
class StoragePublisher {
public:
  using Clock = std::chrono::steady_clock;
  struct Sink {
    std::function<void(uint64_t revision, std::vector<uint8_t> image)> publish;
    std::function<void(bool pending)> pending;
  };
  struct Options {
    Clock::duration settle;
    Clock::duration poll;
  };
  StoragePublisher(std::shared_ptr<const TemporaryFilesystem> files, std::function<std::vector<uint8_t>(uint64_t*)> encode, Sink sink, Options options);
  ~StoragePublisher();
  // the publisher thread calls this every poll interval; with a zero interval there is no thread and the caller polls
  void Poll(Clock::time_point now);

private:
  std::shared_ptr<const TemporaryFilesystem> files_;
  std::function<std::vector<uint8_t>(uint64_t*)> encode_;
  Sink sink_;
  Options options_;
  std::mutex lock_;
  std::condition_variable wake_;
  bool stopping_ = false;
  bool pending_ = false;
  uint64_t seen_;
  Clock::time_point changed_{};
  std::thread thread_;
};

}
