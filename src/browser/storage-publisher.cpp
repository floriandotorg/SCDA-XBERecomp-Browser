#include "browser/storage-publisher.h"
#include <algorithm>

namespace browser {

namespace {

constexpr std::array<uint8_t, 8> ImageMagic{'S', 'C', 'D', 'A', 'U', 'S', 'E', 'R'};
constexpr uint32_t ImageVersion = 1;
constexpr size_t HeaderBytes = ImageMagic.size() + 4 + 16 + 20;

}

std::vector<uint8_t> EncodeStorageImage(const ConsoleIdentity& identity, const TemporaryFilesystem& files, uint64_t* revision) {
  const auto contents = files.Serialize(revision);
  std::vector<uint8_t> image;
  image.reserve(HeaderBytes + contents.size());
  image.insert(image.end(), ImageMagic.begin(), ImageMagic.end());
  for (size_t n = 0; n < 4; ++n) { image.push_back(uint8_t(ImageVersion >> (8 * n))); }
  image.insert(image.end(), identity.driveKey.begin(), identity.driveKey.end());
  image.insert(image.end(), identity.serial.begin(), identity.serial.end());
  image.insert(image.end(), contents.begin(), contents.end());
  return image;
}

uint32_t DecodeStorageImage(std::span<const uint8_t> image, ConsoleIdentity& identity, TemporaryFilesystem& files) {
  constexpr uint32_t invalid = 0xc000000d;
  if (image.size() < HeaderBytes || !std::equal(ImageMagic.begin(), ImageMagic.end(), image.begin())) { return invalid; }
  const uint32_t version = image[8] | image[9] << 8 | image[10] << 16 | uint32_t(image[11]) << 24;
  if (version != ImageVersion) { return invalid; }
  ConsoleIdentity decoded;
  std::copy_n(image.begin() + 12, decoded.driveKey.size(), decoded.driveKey.begin());
  std::copy_n(image.begin() + 28, decoded.serial.size(), decoded.serial.begin());
  if (!std::all_of(decoded.serial.begin(), decoded.serial.end(), [](uint8_t digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F');
      })) { return invalid; }
  if (const auto status = files.Restore(image.subspan(HeaderBytes))) { return status; }
  identity = decoded;
  return 0;
}

StoragePublisher::StoragePublisher(std::shared_ptr<const TemporaryFilesystem> files, std::function<std::vector<uint8_t>(uint64_t*)> encode, Sink sink, Options options)
  : files_(std::move(files)), encode_(std::move(encode)), sink_(std::move(sink)), options_(options), seen_(files_->Revision()) {
  if (options_.poll <= Clock::duration::zero()) { return; }
  thread_ = std::thread([this] {
    std::unique_lock guard(lock_);
    while (!stopping_) {
      guard.unlock();
      try { Poll(Clock::now()); } catch (...) {}
      guard.lock();
      wake_.wait_for(guard, options_.poll, [this] { return stopping_; });
    }
  });
}

StoragePublisher::~StoragePublisher() {
  {
    std::lock_guard guard(lock_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) { thread_.join(); }
}

void StoragePublisher::Poll(Clock::time_point now) {
  const auto revision = files_->Revision();
  if (revision != seen_) {
    seen_ = revision;
    changed_ = now;
    if (!pending_) {
      pending_ = true;
      sink_.pending(true);
    }
  }
  if (!pending_ || files_->Writers() || now - changed_ < options_.settle) { return; }
  uint64_t captured = 0;
  auto image = encode_(&captured);
  if (captured != seen_) {
    seen_ = captured;
    changed_ = now;
    return;
  }
  sink_.publish(captured, std::move(image));
  pending_ = false;
  sink_.pending(false);
}

}
