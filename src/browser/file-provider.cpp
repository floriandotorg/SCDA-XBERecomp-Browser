#include "browser/file-provider.h"
#include "browser/raw-cache.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <map>

namespace browser {
namespace {

class FileProvider {
  Runtime& runtime_;
  PreparedImage& image_;
  std::shared_ptr<const kernel::DiscIndex> disc_;
  std::shared_ptr<TemporaryFilesystem> temporary_;
  std::shared_ptr<RawCache> rawCache_;
  std::mutex lock_;
  DiscReadSnapshot discReads_;
  std::mutex readStagingMutex_;
  std::vector<uint8_t> readStaging_;
  uint32_t maximumReadBytes_;
  uint32_t maximumHandles_;
  std::function<void(FileOpenDiagnostic)> fileOpenDiagnostic_;
  std::function<void(FileOperationDiagnostic)> operationDiagnostic_;
  std::array<uint32_t, 2> operationReports_{};
  struct DiscFile {
    std::shared_ptr<const kernel::DiscIndex> owner;
    const kernel::DiscIndex::Entry* entry;
    std::mutex io;
    uint64_t position = 0;
    DiscFile(std::shared_ptr<const kernel::DiscIndex> disc, const kernel::DiscIndex::Entry* file)
      : owner(std::move(disc)), entry(file) {}
  };
  struct File {
    bool temporary;
    bool directory;
    std::string path;
    bool reads;
    bool writes;
    uint32_t share;
    bool synchronous;
    bool writeData;
    bool raw;
    bool cache;
    uint64_t position = 0;
    bool alertable = false;
    std::shared_ptr<DiscFile> disc;
    std::vector<TemporaryFilesystem::DirectoryEntry> entries;
    std::string pattern;
    size_t nextEntry = 0;
    bool listed = false;
    bool deletes = false;
    bool deletePending = false;
    bool writeAttributes = false;
  };
  std::map<uint32_t, File> files_;
  std::map<uint32_t, std::string> links_;
  std::map<std::string, std::string> aliases_ = {{"D:", "\\DEVICE\\CDROM0"}};
  uint32_t nextHandle_ = 0x71000004;

  static bool ValidPath(const std::string& path) {
    if (path.size() > 240) { return false; }
    size_t start = 0;
    while (start < path.size()) {
      const auto end = path.find('\\', start);
      const auto part = path.substr(start, end == std::string::npos ? end : end - start);
      if (part.empty() || part == "." || part == ".." || part.find_first_of(":*?\"") != std::string::npos) { return false; }
      if (end == std::string::npos) { break; }
      start = end + 1;
      if (start == path.size()) { return false; }
    }
    return true;
  }

  static bool ResolveDevice(std::string& path, bool& temporary, bool* cache = nullptr) {
    for (const std::string prefix : {"\\DEVICE\\CDROM0", "\\DEVICE\\HARDDISK0\\PARTITION1", "\\DEVICE\\HARDDISK0\\PARTITION3"}) {
      if (path.starts_with(prefix) && (path.size() == prefix.size() || path[prefix.size()] == '\\')) {
        temporary = prefix != "\\DEVICE\\CDROM0";
        if (cache) { *cache = prefix == "\\DEVICE\\HARDDISK0\\PARTITION3"; }
        path.erase(0, prefix.size());
        if (!path.empty()) { path.erase(0, 1); }
        return true;
      }
    }
    return false;
  }

  uint32_t ReadName(const X86Context& ctx, uint32_t address, std::string& path, std::string* originalPath = nullptr) {
    std::array<uint32_t, 2> descriptor;
    image_.memory->Read(ctx, address, descriptor.data(), sizeof(descriptor));
    const uint32_t length = descriptor[0] & 0xffff, maximum = descriptor[0] >> 16;
    if (!length || length > maximum || length > 240 || !descriptor[1]) { return 0xc0000033; }
    path.resize(length);
    image_.memory->Read(ctx, descriptor[1], path.data(), length);
    if (originalPath) { *originalPath = path; }
    for (char& value : path) {
      if (uint8_t(value) < 0x20 || uint8_t(value) >= 0x7f || value == '/') { return 0xc0000033; }
      if (value >= 'a' && value <= 'z') { value -= 'a' - 'A'; }
    }
    return 0;
  }

  void RequireOwner(const X86Context& ctx, uint8_t* base) {
    if (ctx.host != &runtime_ || !image_.importsBound || image_.boundRuntime != &runtime_ ||
        base != image_.memory->Base() || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
      throw Fault(ctx, 202, 0, "file service requires its owned image worker without a backing lease");
    }
  }

  std::shared_ptr<TemporaryFilesystem> Storage(const File& file) const {
    return file.cache ? rawCache_->Partition(RawCache::FirstPartition)->Filesystem() : temporary_;
  }

  void ReportOperationLocked(uint32_t handle, uint32_t ordinal, uint32_t information, uint32_t status, uint32_t length) {
    if (!status || status == 0xc0000011 || !operationDiagnostic_) { return; }
    auto& count = operationReports_[ordinal == 226 ? 0 : 1];
    if (count == 32) { return; }
    ++count;
    const auto file = files_.find(handle);
    try { operationDiagnostic_({ordinal, information, status, length, file == files_.end() ? "" : file->second.path}); }
    catch (...) {}
  }

public:
  void InstallData(std::vector<KernelDataExport>& exports) {
    constexpr uint32_t address = 0xc0001000;
    constexpr uint32_t descriptorAddress = address + 4;
    constexpr uint32_t nameAddress = descriptorAddress + 8;
    constexpr char name[] = "\\Device\\CdRom0\\default.xbe";
    constexpr std::array<uint32_t, 2> descriptor = {
      uint32_t(sizeof(name) - 1) | (uint32_t(sizeof(name)) << 16), nameAddress
    };
    exports.reserve(exports.size() + 2);
    image_.memory->Map(address, 4096);
    image_.memory->Commit(address, 4096);
    const uint32_t count = RawCache::PartitionCount;
    image_.memory->Write({}, address, &count, sizeof(count));
    image_.memory->Write({}, descriptorAddress, descriptor.data(), sizeof(descriptor));
    image_.memory->Write({}, nameAddress, name, sizeof(name));
    exports.push_back({40, address, sizeof(count)});
    exports.push_back({326, descriptorAddress, sizeof(descriptor)});
  }

  FileProvider(Runtime& runtime, PreparedImage& image, std::shared_ptr<const kernel::DiscIndex> disc,
      std::shared_ptr<TemporaryFilesystem> temporary, std::function<void(FileOpenDiagnostic)> fileOpenDiagnostic, std::shared_ptr<RawCache> cache,
      std::function<void(FileOperationDiagnostic)> operationDiagnostic, uint32_t maximumReadBytes, uint32_t maximumHandles)
    : runtime_(runtime), image_(image), disc_(std::move(disc)),
      temporary_(temporary ? std::move(temporary) : std::make_shared<TemporaryFilesystem>()),
      rawCache_(cache ? std::move(cache) : std::make_shared<RawCache>()), maximumReadBytes_(maximumReadBytes), maximumHandles_(maximumHandles),
      fileOpenDiagnostic_(std::move(fileOpenDiagnostic)), operationDiagnostic_(std::move(operationDiagnostic)) {
    if (maximumReadBytes < (1u << 20) || maximumReadBytes > (32u << 20) || (maximumReadBytes & (maximumReadBytes - 1))) {
      throw Fault({}, 202, maximumReadBytes, "file read budget must be a power of two between one and thirty-two MiB");
    }
    if (maximumHandles != 64 && maximumHandles != 256) {
      throw Fault({}, 202, maximumHandles, "file handle budget must select the default or story profile");
    }
  }

  DiscReadSnapshot DiscReads() {
    std::lock_guard lock(lock_);
    return discReads_;
  }

  bool Contains(uint32_t handle) {
    std::lock_guard lock(lock_);
    return links_.contains(handle) || files_.contains(handle);
  }

  bool Close(uint32_t handle) {
    std::lock_guard lock(lock_);
    if (links_.erase(handle)) { return true; }
    const auto file = files_.find(handle);
    if (file == files_.end()) { return false; }
    if (file->second.raw && file->second.cache) { rawCache_->Partition(RawCache::FirstPartition)->CloseRaw(); }
    if (file->second.deletePending) {
      const auto& selected = file->second;
      const bool retained = std::any_of(files_.begin(), files_.end(), [&](const auto& entry) {
        return entry.first != handle && entry.second.temporary && entry.second.cache == selected.cache && entry.second.path == selected.path;
      });
      if (!retained && Storage(selected)->Delete(selected.path)) { throw Fault({}, handle, 0, "temporary deletion failed at final close"); }
    }
    files_.erase(file);
    return true;
  }

  void Open(X86Context& ctx, uint8_t* base, bool create = false) {
    RequireOwner(ctx, base);
    auto& memory = *image_.memory;
    auto transition = memory.Mutate();
    runtime_.Checkpoint(ctx);
    std::array<uint32_t, 10> frame{};
    const uint32_t frameSize = create ? 40 : 28;
    if (uint64_t(ctx.esp) + frameSize > UINT32_MAX) { throw Fault(ctx, ctx.esp, frameSize, "open frame overflow"); }
    memory.Read(ctx, ctx.esp, frame.data(), frameSize);
    const uint32_t output = frame[1], access = frame[2], attributes = frame[3], statusOutput = frame[4];
    const uint32_t share = frame[create ? 7 : 5], options = frame[create ? 9 : 6];
    const uint32_t disposition = create ? frame[8] : 1;
    uint64_t allocation = 0;
    if (create && frame[5]) { memory.Read(ctx, frame[5], &allocation, sizeof(allocation)); }
    const auto overlaps = [](uint32_t first, uint32_t size, uint32_t second, uint32_t length) {
      return uint64_t(first) < uint64_t(second) + length && uint64_t(second) < uint64_t(first) + size;
    };
    if (!output || !statusOutput || (output & 3) || (statusOutput & 3) ||
        overlaps(output, 4, statusOutput, 8) || overlaps(output, 4, ctx.esp, frameSize) ||
        overlaps(statusOutput, 8, ctx.esp, frameSize)) {
      throw Fault(ctx, output, 4, "invalid open output spans");
    }
    memory.CheckRange(ctx, output, 4);
    memory.CheckRange(ctx, statusOutput, 8);
    std::array<uint32_t, 3> object;
    memory.Read(ctx, attributes, object.data(), sizeof(object));
    std::string path, originalPath;
    uint32_t status = ReadName(ctx, object[1], path, fileOpenDiagnostic_ ? &originalPath : nullptr);
    std::lock_guard lock(lock_);
    bool temporary = false;
    bool raw = false;
    bool cache = false;
    bool directory = false;
    bool createEntry = false;
    bool overwriteEntry = false;
    const bool streamingWrite = create && disposition == 5 && options == 0x48 && access == 0x40100080 && frame[6] == 0;
    const bool overwrite = create && disposition == 5 && (options == 0x60 || options == 0x64 || streamingWrite) && access == 0x40100080;
    const bool noReadCache = create && disposition == 1 && options == 0x6c && access == 0x80100080 && frame[6] == 0x80;
    const bool readMetadata = create && disposition == 3 && options == 0x60 && access == 0x80100080 && frame[6] == 4;
    const bool writableOpenIf = create && disposition == 3 && options == 0x60 &&
      (access == 0x40100080 || access == 0xc0100080) && frame[6] == 0x80;
    const bool deleteOpen = create && disposition == 1 && (options == 0x60 || options == 0x64) && access == 0x110080;
    const bool readonlyOpen = create && disposition == 1 && (options == 0x60 || options == 0x64) && access == 0x80100080 && frame[6] == 1;
    const bool streamingRead = create && disposition == 1 && options == 0x48 && access == 0x80100080 && frame[6] == 0;
    if (!status && create && ((allocation && (options != 0x22 || (disposition != 2 && disposition != 3))) ||
        (!readonlyOpen && (frame[6] & ~0x94u)) || disposition < 1 || (disposition > 3 && !overwrite))) { status = 0xc00000bb; }
    if (!status && create && !overwrite && !noReadCache && !readMetadata && !writableOpenIf && !deleteOpen && !streamingRead && !(options & 1) && options != 0x22 &&
        (disposition != 1 || (options != 0x60 && options != 0x64) || !(access & 0x80000001u) || (access & ~0x80120089u))) { status = 0xc00000bb; }
    if (!status && create && !(options & 1) && (frame[6] & 0x10)) { status = 0xc000000d; }
    const uint32_t allowedOptions = (create ? 0x804067u : 0x804079u) | (noReadCache || streamingRead || streamingWrite ? 8u : 0u);
    if (!status && (object[2] & ~0x40u || share & ~7u || options & ~allowedOptions ||
        (options & 0x41) == 0x41 || (options & 0x30) == 0x30)) { status = 0xc00000bb; }
    if (!status) {
      if (object[0] && object[0] != 0xfffffffdu) {
        const auto root = files_.find(object[0]);
        if (root == files_.end()) { status = 0xc0000008; }
        else if (!root->second.directory) { status = 0xc0000024; }
        else if (path.front() == '\\' || path.find(':') != std::string::npos) { status = 0xc0000033; }
        else {
          temporary = root->second.temporary;
          cache = root->second.cache;
          path = root->second.path + (root->second.path.empty() ? "" : "\\") + path;
        }
      } else {
        auto aliasPath = path.starts_with("\\??\\") ? path.substr(4) : path;
        const bool drivePath = aliasPath.size() >= 3 && aliasPath[1] == ':' && aliasPath[2] == '\\';
        if (object[0] == 0xfffffffdu && !drivePath && !path.starts_with("\\DEVICE\\")) { status = 0xc0000033; }
        if (!status && aliasPath.size() >= 2 && aliasPath[1] == ':' && (aliasPath.size() == 2 || aliasPath[2] == '\\')) {
          const auto alias = aliases_.find(aliasPath.substr(0, 2));
          if (alias == aliases_.end()) { status = 0xc00000bb; }
          else { path = alias->second + aliasPath.substr(2); }
        }
        cache = !status && path == "\\DEVICE\\HARDDISK0\\PARTITION3" && options == 0x18;
        raw = !status && (path == "\\DEVICE\\HARDDISK0\\PARTITION0" || cache);
        if (!status && !raw && !ResolveDevice(path, temporary, &cache)) { status = 0xc00000bb; }
      }
    }
    auto storage = cache ? rawCache_->Partition(RawCache::FirstPartition)->Filesystem() : temporary_;
    if (!status && !raw && (options & 0x18) && !streamingRead && !streamingWrite && (!noReadCache || temporary)) { status = 0xc00000bb; }
    if (!status && !raw && (options & 1) && path.size() > 1 && path.back() == '\\' && path[path.size() - 2] != '\\') {
      path.pop_back();
    }
    if (!status && !raw && !ValidPath(path)) { status = 0xc0000033; }
    if (!status && raw && (create || access != (cache ? 0x100003u : 0xc0100000u) ||
        options != (cache ? 0x18u : 0x10u) || (cache && share))) { status = 0xc00000bb; }
    if (!status && allocation && (!temporary || raw || (options & 1))) { status = 0xc00000bb; }
    if (!status && allocation >> 63) { status = 0xc000000d; }
    const uint32_t allowedAccess = temporary && !raw ? 0xc013019fu : raw ? 0xc012019fu : 0x80120089u;
    if (!status && deleteOpen && !temporary) { status = 0xc00000bb; }
    if (!status && (access & ~allowedAccess)) { status = 0xc0000022; }
    if (!status && create && !temporary && disposition != 1) { status = 0xc0000022; }
    if (!status && (options & 0x20) && !(access & 0xc0100000u)) { status = 0xc000000d; }
    if (!status && !temporary && !raw && !disc_) { status = 0xc00000a3; }
    if (!status && cache && !raw && !storage) {
      runtime_.Checkpoint(ctx);
      status = rawCache_->Partition(RawCache::FirstPartition)->Mount();
      if (!status) {
        storage = rawCache_->Partition(RawCache::FirstPartition)->Filesystem();
        if (!storage) { throw Fault(ctx, 202, 0, "cache mount did not retain its namespace"); }
      }
    }
    if (!status) {
      const auto separator = path.rfind('\\');
      const auto parentPath = separator == std::string::npos ? "" : path.substr(0, separator);
      if (raw) {
        directory = false;
      } else if (temporary) {
        const auto entry = storage->Stat(path);
        if (!entry) {
          const auto parent = storage->Stat(parentPath);
          if (!parent || !parent->directory) { status = 0xc000003a; }
          else if (create && disposition != 1) { directory = options & 1; createEntry = true; }
          else { status = 0xc0000034; }
        } else {
          directory = entry->directory;
          if (create && disposition == 2) { status = 0xc0000035; }
          if (!directory && (entry->attributes & 1) && (access & 0x40010006u)) { status = 0xc0000022; }
          overwriteEntry = overwrite;
        }
      } else {
        const auto* entry = disc_->Find(path);
        if (!entry) {
          const auto* parent = disc_->Find(parentPath);
          status = parent && parent->dir ? 0xc0000034 : 0xc000003a;
        } else { directory = entry->dir; }
      }
      if (!status && directory && (options & 0x40)) { status = 0xc00000ba; }
      if (!status && !directory && (options & 1)) { status = 0xc0000103; }
      if (!status && (files_.size() + links_.size() >= maximumHandles_ || nextHandle_ > 0x71fffffc)) { status = 0xc000009a; }
    }
    const bool reads = (access & 0x80000001u) != 0;
    const bool writes = (access & 0x40000006u) != 0;
    const bool deletes = (access & 0x10000u) != 0;
    if (!status) {
      for (const auto& [handle, file] : files_) {
        if (file.temporary != temporary || file.raw != raw || file.cache != cache || file.path != path) { continue; }
        if (file.deletePending) { status = 0xc0000056; break; }
        if ((reads && !(file.share & 1)) || (file.reads && !(share & 1)) ||
             (writes && !(file.share & 2)) || (file.writes && !(share & 2)) ||
             (deletes && !(file.share & 4)) || (file.deletes && !(share & 4))) {
          status = 0xc0000043;
          break;
        }
      }
    }
    runtime_.Checkpoint(ctx);
    if (!status && raw && cache) { status = rawCache_->Partition(RawCache::FirstPartition)->OpenRaw(); }
    const uint32_t fileAttributes = overwrite && !frame[6] ? 0x80 : frame[6];
    if (!status && createEntry) { status = directory ? storage->CreateDirectory(path) : storage->CreateFile(path, fileAttributes, allocation); }
    if (!status && overwriteEntry) { status = storage->OverwriteFile(path, fileAttributes); }
    if (!status) {
      const uint32_t handle = nextHandle_;
      File file{temporary, directory, path, reads, writes, share, bool(options & 0x30), bool(access & 0x40000002u), raw, cache};
      file.alertable = options & 0x10;
      file.deletes = deletes;
      file.writeAttributes = (access & 0x40000100u) != 0;
      if (!temporary && !raw) { file.disc = std::make_shared<DiscFile>(disc_, disc_->Find(path)); }
      files_.emplace(handle, std::move(file));
      nextHandle_ += 4;
      memory.Write(ctx, output, &handle, 4);
    }
    const std::array<uint32_t, 2> result = {status, status ? 0u : createEntry ? 2u : overwriteEntry ? 3u : 1u};
    memory.Write(ctx, statusOutput, result.data(), sizeof(result));
    ctx.eax = status;
    ctx.esp += frameSize;
    if (fileOpenDiagnostic_) {
      try {
        fileOpenDiagnostic_({std::move(originalPath), path.substr(0, 240), status, object[0], disposition, options, access, create ? frame[6] : 0, allocation});
      } catch (...) {}
    }
  }

  void OpenLink(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    auto& memory = *image_.memory;
    auto transition = memory.Mutate();
    runtime_.Checkpoint(ctx);
    std::array<uint32_t, 3> frame{};
    if (ctx.esp > UINT32_MAX - sizeof(frame)) { throw Fault(ctx, ctx.esp, sizeof(frame), "link open frame overflow"); }
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    if (!frame[1] || (frame[1] & 3) || (uint64_t(frame[1]) < uint64_t(ctx.esp) + sizeof(frame) && uint64_t(frame[1]) + 4 > ctx.esp)) {
      throw Fault(ctx, frame[1], 4, "invalid link handle output");
    }
    memory.CheckRange(ctx, frame[1], 4);
    std::array<uint32_t, 3> attributes{};
    memory.Read(ctx, frame[2], attributes.data(), sizeof(attributes));
    std::string name;
    uint32_t status = attributes[0] || attributes[2] != 0x40 ? 0xc00000bb : ReadName(ctx, attributes[1], name);
    if (!status && (!name.starts_with("\\??\\") || name.size() != 6 || name[5] != ':')) { status = 0xc0000033; }
    std::lock_guard lock(lock_);
    const auto alias = aliases_.find(name.size() == 6 ? name.substr(4) : "");
    if (!status && alias == aliases_.end()) { status = 0xc0000034; }
    if (!status && (files_.size() + links_.size() >= maximumHandles_ || nextHandle_ > 0x71fffffc)) { status = 0xc000009a; }
    runtime_.Checkpoint(ctx);
    if (!status) {
      const uint32_t handle = nextHandle_;
      links_.emplace(handle, alias->second);
      nextHandle_ += 4;
      memory.Write(ctx, frame[1], &handle, sizeof(handle));
    }
    ctx.eax = status;
    ctx.esp += sizeof(frame);
  }

  void QueryLink(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    auto& memory = *image_.memory;
    auto transition = memory.Mutate();
    runtime_.Checkpoint(ctx);
    std::array<uint32_t, 4> frame{};
    if (ctx.esp > UINT32_MAX - sizeof(frame)) { throw Fault(ctx, ctx.esp, sizeof(frame), "link query frame overflow"); }
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    const auto overlaps = [](uint32_t first, uint32_t size, uint32_t second, uint32_t count) {
      return size && count && uint64_t(first) < uint64_t(second) + count && uint64_t(second) < uint64_t(first) + size;
    };
    if (!frame[2] || (frame[2] & 3) || (frame[3] & 3) || overlaps(frame[2], 8, ctx.esp, sizeof(frame)) ||
        (frame[3] && (overlaps(frame[3], 4, frame[2], 8) || overlaps(frame[3], 4, ctx.esp, sizeof(frame))))) {
      throw Fault(ctx, frame[2], 8, "invalid link query descriptor outputs");
    }
    std::array<uint32_t, 2> descriptor{};
    memory.Read(ctx, frame[2], descriptor.data(), sizeof(descriptor));
    if (frame[3]) { memory.CheckRange(ctx, frame[3], 4); }
    std::lock_guard lock(lock_);
    const auto link = links_.find(frame[1]);
    uint32_t status = link == links_.end() ? 0xc0000008 : 0;
    const uint32_t length = status ? 0 : uint32_t(link->second.size());
    if (!status && length > descriptor[0] >> 16) { status = 0xc0000023; }
    if (!status) {
      if (!descriptor[1] || overlaps(descriptor[1], length, frame[2], 8) || overlaps(descriptor[1], length, ctx.esp, sizeof(frame)) ||
          (frame[3] && overlaps(descriptor[1], length, frame[3], 4))) { throw Fault(ctx, descriptor[1], length, "invalid link query byte output"); }
      memory.CheckRange(ctx, descriptor[1], length);
    }
    runtime_.Checkpoint(ctx);
    if (link != links_.end() && frame[3]) { memory.Write(ctx, frame[3], &length, sizeof(length)); }
    if (!status) {
      memory.Write(ctx, descriptor[1], link->second.data(), length);
      const uint16_t countedLength = uint16_t(length);
      memory.Write(ctx, frame[2], &countedLength, sizeof(countedLength));
    }
    ctx.eax = status;
    ctx.esp += sizeof(frame);
  }

  void CreateAlias(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    auto transition = image_.memory->Mutate();
    runtime_.Checkpoint(ctx);
    std::array<uint32_t, 3> frame;
    if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "alias frame overflow"); }
    image_.memory->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    std::string name, target;
    uint32_t status = ReadName(ctx, frame[1], name);
    if (!status) { status = ReadName(ctx, frame[2], target); }
    if (!status && (name.size() != 6 || !name.starts_with("\\??\\") || name[4] < 'A' || name[4] > 'Z' || name[5] != ':')) { status = 0xc0000033; }
    bool temporary = false;
    auto relative = target;
    if (!status && !ResolveDevice(relative, temporary)) { status = 0xc00000bb; }
    if (!status && !ValidPath(relative)) { status = 0xc0000033; }
    std::lock_guard lock(lock_);
    const auto alias = name.size() == 6 ? name.substr(4) : "";
    if (!status && aliases_.contains(alias)) { status = 0xc0000035; }
    runtime_.Checkpoint(ctx);
    if (!status) {
      if (target.back() == '\\') { target.pop_back(); }
      aliases_.emplace(alias, target);
    }
    ctx.eax = status;
    ctx.esp += sizeof(frame);
  }

  void DeleteAlias(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    auto transition = image_.memory->Mutate();
    runtime_.Checkpoint(ctx);
    std::array<uint32_t, 2> frame{};
    if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "alias deletion frame overflow"); }
    image_.memory->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    std::string name;
    uint32_t status = ReadName(ctx, frame[1], name);
    if (!status && (name.size() != 6 || !name.starts_with("\\??\\") || name[4] < 'A' || name[4] > 'Z' || name[5] != ':')) {
      status = 0xc0000033;
    }
    std::lock_guard lock(lock_);
    runtime_.Checkpoint(ctx);
    if (!status && !aliases_.erase(name.substr(4))) { status = 0xc0000034; }
    ctx.eax = status;
    ctx.esp += sizeof(frame);
  }

  void DeviceControl(X86Context& ctx, uint8_t* base, bool filesystem = false) {
    RequireOwner(ctx, base);
    auto& memory = *image_.memory;
    auto transition = memory.Mutate();
    runtime_.Checkpoint(ctx);
    std::array<uint32_t, 11> frame{};
    if (ctx.esp > UINT32_MAX - sizeof(frame)) { throw Fault(ctx, ctx.esp, sizeof(frame), "device control frame overflow"); }
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    const uint32_t statusOutput = frame[5], output = frame[9], length = frame[10];
    const auto overlaps = [](uint32_t address, uint32_t size, uint32_t other, uint32_t otherSize) {
      return size && otherSize && uint64_t(address) < uint64_t(other) + otherSize && uint64_t(other) < uint64_t(address) + size;
    };
    if (!statusOutput || (statusOutput & 3) || (!output && length)) { throw Fault(ctx, output, length, "invalid device control outputs"); }
    memory.CheckRange(ctx, statusOutput, 8);
    if (length) { memory.CheckRange(ctx, output, length); }
    if (overlaps(statusOutput, 8, output, length) || overlaps(statusOutput, 8, ctx.esp, sizeof(frame)) ||
        overlaps(output, length, ctx.esp, sizeof(frame))) { throw Fault(ctx, statusOutput, length, "invalid device control output span"); }
    std::lock_guard lock(lock_);
    uint32_t status = 0;
    const auto file = files_.find(frame[1]);
    const uint32_t required = filesystem ? 0 : frame[6] == 0x70000 ? 24 : frame[6] == 0x74004 ? 32 : 0;
    if (file == files_.end()) { status = 0xc0000008; }
    else if (!file->second.raw || !file->second.cache || frame[2] || frame[3] || frame[4] || frame[7] || frame[8] ||
        (filesystem ? frame[6] != 0x90020 || output || length : !required)) { status = 0xc00000bb; }
    else if (length < required) { status = 0xc0000023; }
    std::array<uint32_t, 8> result{};
    if (!status && !filesystem) {
      const uint64_t capacity = rawCache_->Partition(RawCache::FirstPartition)->CapacityBytes();
      if (frame[6] == 0x70000) { result = {uint32_t(capacity / (512 * 32)), 0, 12, 1, 32, CacheFatx::SectorSize, 0, 0}; }
      else { result = {0, 0, uint32_t(capacity), 0, 0, RawCache::FirstPartition, 0, 0}; }
    }
    runtime_.Checkpoint(ctx);
    if (!status && filesystem) { status = rawCache_->Partition(RawCache::FirstPartition)->Dismount(); }
    if (!status && required) { memory.Write(ctx, output, result.data(), required); }
    const std::array<uint32_t, 2> ioStatus = {status, status ? 0u : required};
    memory.Write(ctx, statusOutput, ioStatus.data(), sizeof(ioStatus));
    ctx.eax = status;
    ctx.esp += sizeof(frame);
  }

  bool DiscTransfer(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    auto& memory = *image_.memory;
    std::array<uint32_t, 9> frame{};
    std::shared_ptr<DiscFile> selected;
    std::string path;
    uint64_t explicitOffset = 0;
    uint32_t status = 0;
    bool synchronous = true;
    std::function<void()> complete;
    const auto overlaps = [](uint32_t first, uint32_t size, uint32_t second, uint32_t count) {
      return size && count && uint64_t(first) < uint64_t(second) + count && uint64_t(second) < uint64_t(first) + size;
    };
    const auto validate = [&] {
      const uint32_t output = frame[6], length = frame[7], statusOutput = frame[5];
      if (!statusOutput || (statusOutput & 3) || (!output && length) ||
          overlaps(statusOutput, 8, ctx.esp, sizeof(frame)) || overlaps(output, length, statusOutput, 8) ||
          overlaps(output, length, ctx.esp, sizeof(frame))) {
        throw Fault(ctx, output, length, "invalid disc read output spans");
      }
      memory.CheckRange(ctx, statusOutput, 8);
      if (length) { memory.CheckRange(ctx, output, length); }
      if (frame[8]) {
        if (overlaps(frame[8], 8, statusOutput, 8) || overlaps(frame[8], 8, output, length) ||
            overlaps(frame[8], 8, ctx.esp, sizeof(frame))) {
          throw Fault(ctx, frame[8], 8, "invalid disc read offset span");
        }
        memory.CheckRange(ctx, frame[8], 8);
      }
    };
    {
      auto transition = memory.Mutate();
      runtime_.Checkpoint(ctx);
      if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "read frame overflow"); }
      memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      std::lock_guard lock(lock_);
      const auto file = files_.find(frame[1]);
      if (file != files_.end() && (file->second.raw || file->second.temporary)) { return false; }
      validate();
      if (frame[8]) { memory.Read(ctx, frame[8], &explicitOffset, 8); }
      if (file == files_.end()) { status = 0xc0000008; }
      else if (file->second.directory) { status = 0xc00000ba; }
      else if (!file->second.reads) { status = 0xc0000022; }
      else if (file->second.alertable || frame[3] ||
          (file->second.synchronous ? bool(frame[2] || frame[4]) : !frame[8])) { status = 0xc00000bb; }
      else if (frame[7] > maximumReadBytes_ || (frame[8] && (explicitOffset >> 63) && explicitOffset != UINT64_MAX - 1)) { status = 0xc000000d; }
      else {
        synchronous = file->second.synchronous;
        if (!synchronous && explicitOffset >> 63) { status = 0xc000000d; }
        if (!status && frame[2]) {
          if (!image_.beginFileIo) { status = 0xc00000bb; }
          else {
            auto event = image_.beginFileIo(frame[2] & ~1u);
            status = event.status;
            complete = std::move(event.complete);
          }
        }
        if (!status) { selected = file->second.disc; path = file->second.path; }
      }
      if (status) {
        runtime_.Checkpoint(ctx);
        ReportOperationLocked(frame[1], 219, 0, status, frame[7]);
        const std::array<uint32_t, 2> result = {status, 0};
        memory.Write(ctx, frame[5], result.data(), sizeof(result));
        ctx.eax = status;
        ctx.esp += sizeof(frame);
        return true;
      }
    }
    if (!selected || !selected->entry) { throw Fault(ctx, frame[1], 0, "disc handle lost its owned metadata"); }
    const auto readStarted = std::chrono::steady_clock::now();
    std::unique_lock serial(selected->io);
    runtime_.Checkpoint(ctx);
    const uint64_t offset = !frame[8] || explicitOffset == UINT64_MAX - 1 ? selected->position : explicitOffset;
    uint32_t count = 0;
    if (frame[7]) {
      if (offset >= selected->entry->size) { status = 0xc0000011; }
      else { count = uint32_t(std::min<uint64_t>(frame[7], selected->entry->size - offset)); }
    }
    {
      std::lock_guard lock(lock_);
      discReads_.lastPath = path;
      discReads_.lastOffset = offset;
      discReads_.lastBytes = count;
    }
    std::lock_guard stagingLock(readStagingMutex_);
    if (readStaging_.capacity() < count) { std::vector<uint8_t>(count).swap(readStaging_); }
    readStaging_.resize(count);
    auto& bytes = readStaging_;
    uint32_t failedOffset = 0, failedBytes = count;
    if (count) {
      try {
        constexpr uint32_t chunkBytes = 1u << 20;
        for (uint32_t n = 0; n < (count + chunkBytes - 1) / chunkBytes; ++n) {
          runtime_.Checkpoint(ctx);
          failedOffset = n * chunkBytes;
          failedBytes = std::min(count - failedOffset, chunkBytes);
          if (selected->owner->Read(*selected->entry, offset + failedOffset, bytes.data() + failedOffset, failedBytes) != failedBytes) { status = 0xc0000185; break; }
        }
      } catch (const Cancelled&) {
        throw;
      } catch (...) {
        status = 0xc0000185;
      }
    }
    {
      const auto elapsed = std::chrono::steady_clock::now() - readStarted;
      std::lock_guard lock(lock_);
      discReads_.waitMicroseconds += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }
    if (status == 0xc0000185) {
      runtime_.Checkpoint(ctx);
      const std::string reason = "ISO read failed: " + path + " offset=" + std::to_string(offset + failedOffset) + " bytes=" + std::to_string(failedBytes);
      throw Fault(ctx, frame[6], count, reason.c_str());
    }
    {
      auto transition = memory.Mutate();
      RequireOwner(ctx, base);
      runtime_.Checkpoint(ctx);
      validate();
      std::lock_guard lock(lock_);
      const auto file = files_.find(frame[1]);
      if (file == files_.end() || file->second.disc != selected) { status = 0xc0000008; }
      runtime_.Checkpoint(ctx);
      if (!status && count) {
        memory.Write(ctx, frame[6], bytes.data(), count);
        if (synchronous) {
          selected->position = offset + count;
          file->second.position = selected->position;
        }
        if (file->second.path.ends_with(".XBE")) {
          discReads_.executableBytes += count;
          ++discReads_.executableOperations;
        } else {
          discReads_.assetBytes += count;
          ++discReads_.assetOperations;
        }
      }
      const std::array<uint32_t, 2> result = {status, status ? 0u : count};
      memory.Write(ctx, frame[5], result.data(), sizeof(result));
      ctx.eax = status;
      ctx.esp += sizeof(frame);
    }
    if (complete) { complete(); }
    return true;
  }

  void QueryDirectory(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    auto& memory = *image_.memory;
    auto transition = memory.Mutate();
    runtime_.Checkpoint(ctx);
    std::array<uint32_t, 11> frame;
    if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "directory frame overflow"); }
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    const uint32_t statusOutput = frame[5], output = frame[6], length = frame[7];
    const auto overlaps = [](uint32_t address, uint32_t size, uint32_t other, uint32_t otherSize) {
      return size && otherSize && uint64_t(address) < uint64_t(other) + otherSize && uint64_t(other) < uint64_t(address) + size;
    };
    if (!statusOutput || (statusOutput & 3) || (!output && length) || (output & 3) ||
        overlaps(statusOutput, 8, ctx.esp, sizeof(frame)) || overlaps(output, length, statusOutput, 8) ||
        overlaps(output, length, ctx.esp, sizeof(frame))) { throw Fault(ctx, output, length, "invalid directory output spans"); }
    memory.CheckRange(ctx, statusOutput, 8);
    if (length) { memory.CheckRange(ctx, output, length); }
    std::lock_guard lock(lock_);
    const auto file = files_.find(frame[1]);
    uint32_t status = 0, information = 0;
    if (file == files_.end()) { status = 0xc0000008; }
    else if (!file->second.directory) { status = 0xc0000024; }
    else if (!file->second.reads) { status = 0xc0000022; }
    else if (!file->second.synchronous || file->second.alertable || frame[2] || frame[3] || frame[4] || frame[8] != 1) { status = 0xc00000bb; }
    else if (length > (1u << 20)) { status = 0xc000000d; }
    else if (length < 64) { status = 0xc0000004; }
    const bool first = !status && !file->second.listed;
    const bool refresh = !status && (first || (frame[10] & 0xff));
    std::string pattern = first ? "*" : !status ? file->second.pattern : "";
    if (first && frame[9]) {
      std::array<uint32_t, 2> descriptor;
      if (overlaps(frame[9], 8, statusOutput, 8) || overlaps(frame[9], 8, output, length)) {
        throw Fault(ctx, frame[9], 8, "directory pattern descriptor overlaps output");
      }
      memory.Read(ctx, frame[9], descriptor.data(), sizeof(descriptor));
      const uint32_t patternLength = descriptor[0] & 0xffff;
      if (overlaps(descriptor[1], patternLength, statusOutput, 8) || overlaps(descriptor[1], patternLength, output, length)) {
        throw Fault(ctx, descriptor[1], patternLength, "directory pattern bytes overlap output");
      }
      status = ReadName(ctx, frame[9], pattern);
      if (!status && (pattern == "." || pattern == ".." || pattern.find_first_of("\\/:\"<>") != std::string::npos)) { status = 0xc0000033; }
    }
    const auto matches = [&](const std::string& name) {
      size_t n = 0, p = 0, star = std::string::npos, mark = 0;
      while (n < name.size()) {
        char value = name[n];
        if (value >= 'a' && value <= 'z') { value -= 'a' - 'A'; }
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value)) { ++p; ++n; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p; ++p; mark = n; }
        else if (star != std::string::npos) { p = star + 1; n = ++mark; }
        else { return false; }
      }
      while (p < pattern.size() && pattern[p] == '*') { ++p; }
      return p == pattern.size();
    };
    std::vector<TemporaryFilesystem::DirectoryEntry> snapshot;
    if (!status && refresh) {
      if (file->second.temporary) {
        const auto storage = Storage(file->second);
        auto entries = storage ? storage->List(file->second.path) : std::nullopt;
        if (!entries) { status = 0xc000003a; }
        else { snapshot = std::move(*entries); }
      } else {
        const auto* parent = disc_ ? disc_->Find(file->second.path) : nullptr;
        if (!parent || !parent->dir) { status = 0xc000003a; }
        else if (parent->children.size() > 4096) { status = 0xc000009a; }
        else {
          snapshot.reserve(parent->children.size());
          for (const auto& entry : parent->children) {
            const auto time = disc_->Time();
            snapshot.push_back({entry.name, {entry.dir, entry.dir ? 0 : entry.size, entry.dir ? 0x10u : 0x80u,
              time, time, time, time}});
          }
        }
      }
      if (!status) {
        std::erase_if(snapshot, [&](const auto& entry) { return !matches(entry.name); });
        std::sort(snapshot.begin(), snapshot.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
      }
    }
    std::vector<uint8_t> result;
    bool advance = false;
    if (!status) {
      const auto& entries = refresh ? snapshot : file->second.entries;
      const auto next = refresh ? 0 : file->second.nextEntry;
      if (next >= entries.size()) { status = first ? 0xc000000f : 0x80000006; }
      else {
        const auto& entry = entries[next];
        const auto& metadata = entry.metadata;
        const uint32_t needed = 64 + uint32_t(entry.name.size());
        if (length >= needed || first) {
          information = std::min(length, needed);
          result.resize(information);
          const auto put = [&](size_t at, const auto& value) { std::memcpy(result.data() + at, &value, sizeof(value)); };
          const uint32_t unit = file->second.temporary ? TemporaryFilesystem::AllocationUnit : 4096;
          const uint64_t allocation = metadata.directory ? (file->second.temporary ? unit : 0) :
            (file->second.temporary ? metadata.allocationSize : ((metadata.size + unit - 1) / unit) * unit);
          put(8, metadata.creationTime); put(16, metadata.lastAccessTime); put(24, metadata.lastWriteTime); put(32, metadata.changeTime);
          put(40, metadata.size); put(48, allocation); put(56, metadata.attributes); put(60, uint32_t(entry.name.size()));
          std::memcpy(result.data() + 64, entry.name.data(), information - 64);
          if (length < needed) { status = 0x80000005; }
          else { advance = true; }
        }
      }
    }
    runtime_.Checkpoint(ctx);
    if (information) { memory.Write(ctx, output, result.data(), information); }
    const std::array<uint32_t, 2> ioStatus = {status, information};
    memory.Write(ctx, statusOutput, ioStatus.data(), sizeof(ioStatus));
    if (refresh && (status == 0 || status == 0x80000005 || status == 0xc000000f || status == 0x80000006)) {
      file->second.entries = std::move(snapshot);
      file->second.pattern = std::move(pattern);
      file->second.nextEntry = 0;
      file->second.listed = true;
    }
    if (advance) { ++file->second.nextEntry; }
    ctx.eax = status;
    ctx.esp += sizeof(frame);
  }

  void Transfer(X86Context& ctx, uint8_t* base, bool read = false) {
    if (read && DiscTransfer(ctx, base)) { return; }
    RequireOwner(ctx, base);
    auto& memory = *image_.memory;
    std::function<void()> complete;
    {
      auto transition = memory.Mutate();
      runtime_.Checkpoint(ctx);
      std::array<uint32_t, 9> frame;
      if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "write frame overflow"); }
      memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      const uint32_t statusOutput = frame[5], input = frame[6], length = frame[7];
      const auto overlaps = [](uint32_t first, uint32_t size, uint32_t second, uint32_t count) {
        return uint64_t(first) < uint64_t(second) + count && uint64_t(second) < uint64_t(first) + size;
      };
      if (!statusOutput || (statusOutput & 3) || (!input && length) ||
          overlaps(statusOutput, 8, ctx.esp, sizeof(frame)) || overlaps(input, length, statusOutput, 8) ||
          overlaps(input, length, ctx.esp, sizeof(frame))) { throw Fault(ctx, input, length, "invalid write spans"); }
      memory.CheckRange(ctx, statusOutput, 8);
      std::lock_guard lock(lock_);
      uint32_t status = 0;
      const auto file = files_.find(frame[1]);
      uint64_t offset = 0;
      if (file == files_.end()) { status = 0xc0000008; }
      else if (file->second.raw) {
        if (!file->second.synchronous || frame[2] || frame[3] || frame[4] || !frame[8]) { status = 0xc00000bb; }
        else if ((read && !file->second.reads) || (!read && !file->second.writeData)) { status = 0xc0000022; }
        else if (file->second.cache && read) { status = 0xc00000bb; }
        else {
          if (overlaps(frame[8], 8, statusOutput, 8) || overlaps(frame[8], 8, input, length) ||
              overlaps(frame[8], 8, ctx.esp, sizeof(frame))) { throw Fault(ctx, frame[8], 8, "invalid raw offset span"); }
          memory.Read(ctx, frame[8], &offset, sizeof(offset));
          if (file->second.cache) {
            if (!length || length > CacheFatx::MaximumTransfer || offset % CacheFatx::SectorSize ||
              length % CacheFatx::SectorSize || offset > rawCache_->Partition(RawCache::FirstPartition)->CapacityBytes() ||
              length > rawCache_->Partition(RawCache::FirstPartition)->CapacityBytes() - offset) { status = 0xc000000d; }
          } else if (offset != RawCache::MetadataOffset || length != RawCache::SectorSize) { status = 0xc000000d; }
        }
      }
      else if (!file->second.temporary) { status = 0xc0000022; }
      else if (file->second.directory) { status = 0xc00000ba; }
      else if (read ? !file->second.reads : !file->second.writeData) { status = 0xc0000022; }
      else if (frame[3] || (file->second.synchronous ? bool(frame[2] || frame[4] || frame[8]) : !frame[8])) { status = 0xc00000bb; }
      else if (read && length > maximumReadBytes_) { status = 0xc00000bb; }
      else if (!read && length > Storage(file->second)->Usage().capacity) { status = 0xc000007f; }
      if (!status && !file->second.raw) {
        if (length) { memory.CheckRange(ctx, input, length); }
        offset = file->second.position;
        if (!file->second.synchronous) {
          if (overlaps(frame[8], 8, statusOutput, 8) || overlaps(frame[8], 8, input, length) ||
              overlaps(frame[8], 8, ctx.esp, sizeof(frame))) { throw Fault(ctx, frame[8], 8, "invalid asynchronous offset span"); }
          memory.Read(ctx, frame[8], &offset, sizeof(offset));
          if (offset >> 63) { status = 0xc000000d; }
          if (!status && frame[2]) {
            if (!image_.beginFileIo) { status = 0xc00000bb; }
            else {
              auto event = image_.beginFileIo(frame[2] & ~1u);
              status = event.status;
              complete = std::move(event.complete);
            }
          }
        }
      }
      std::vector<uint8_t> bytes;
      if (!status) {
        if (length) { memory.CheckRange(ctx, input, length); }
        bytes.resize(length);
        if (!read && length) { memory.Read(ctx, input, bytes.data(), length); }
      }
      runtime_.Checkpoint(ctx);
      if (!status && file->second.raw && file->second.cache) { status = rawCache_->Partition(RawCache::FirstPartition)->WriteRaw(offset, bytes); }
      else if (!status && file->second.raw) { status = read ? rawCache_->Read(offset, bytes) : rawCache_->Write(offset, bytes); }
      else if (!status && read) {
        bytes.resize(Storage(file->second)->Read(file->second.path, offset, bytes));
        if (length && bytes.empty()) { status = 0xc0000011; }
      }
      else if (!status) { status = Storage(file->second)->Write(file->second.path, offset, bytes); }
      const uint32_t count = uint32_t(bytes.size());
      if (!status && read && count) { memory.Write(ctx, input, bytes.data(), count); }
      if (!status && file->second.synchronous) { file->second.position = offset + count; }
      ReportOperationLocked(frame[1], read ? 219 : 236, 0, status, length);
      const std::array<uint32_t, 2> result = {status, status ? 0u : count};
      memory.Write(ctx, statusOutput, result.data(), sizeof(result));
      ctx.eax = status;
      ctx.esp += sizeof(frame);
    }
    if (complete) { complete(); }
  }

  void SetInformation(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    auto& memory = *image_.memory;
    std::array<uint32_t, 6> frame{};
    uint64_t position = 0;
    std::shared_ptr<DiscFile> selected;
    const auto overlaps = [](uint32_t first, uint32_t size, uint32_t second, uint32_t count) {
      return size && count && uint64_t(first) < uint64_t(second) + count && uint64_t(second) < uint64_t(first) + size;
    };
    const auto validate = [&] {
      const uint32_t statusOutput = frame[2], input = frame[3], length = frame[4];
      if (!statusOutput || (statusOutput & 3) || !input ||
          overlaps(statusOutput, 8, ctx.esp, sizeof(frame)) || overlaps(input, length, statusOutput, 8) ||
          overlaps(input, length, ctx.esp, sizeof(frame))) { throw Fault(ctx, input, length, "invalid position spans"); }
      memory.CheckRange(ctx, statusOutput, 8);
      if (length) { memory.CheckRange(ctx, input, length); }
    };
    const auto publish = [&](uint32_t status) {
      ReportOperationLocked(frame[1], 226, frame[5], status, frame[4]);
      const std::array<uint32_t, 2> result = {status, 0};
      memory.Write(ctx, frame[2], result.data(), sizeof(result));
      ctx.eax = status;
      ctx.esp += sizeof(frame);
    };
    {
      auto transition = memory.Mutate();
      runtime_.Checkpoint(ctx);
      if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "set information frame overflow"); }
      memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      validate();
      std::lock_guard lock(lock_);
      const auto file = files_.find(frame[1]);
      uint32_t status = 0;
      if (file != files_.end() && (frame[5] == 19 || frame[5] == 20 || frame[5] == 4)) {
        auto& selected = file->second;
        if (!selected.temporary || selected.raw || (frame[5] != 4 ? !selected.writeData : !selected.writeAttributes)) { status = 0xc0000022; }
        else if (frame[4] != (frame[5] != 4 ? 8u : 40u)) { status = 0xc0000004; }
        else if (frame[5] != 4) {
          uint64_t size = 0;
          memory.Read(ctx, frame[3], &size, sizeof(size));
          runtime_.Checkpoint(ctx);
          status = frame[5] == 20 ? Storage(selected)->Resize(selected.path, size) : Storage(selected)->Reserve(selected.path, size);
        } else {
          std::array<uint64_t, 4> times{};
          uint32_t attributes = 0;
          memory.Read(ctx, frame[3], times.data(), sizeof(times));
          memory.Read(ctx, frame[3] + 32, &attributes, sizeof(attributes));
          runtime_.Checkpoint(ctx);
          status = Storage(selected)->SetBasic(selected.path, times, attributes);
        }
        publish(status);
        return;
      }
      if (file != files_.end() && (frame[5] == 13 || frame[5] == 10)) {
        auto& selected = file->second;
        if (!selected.temporary || selected.raw || !selected.deletes) { status = 0xc0000022; }
        else if (selected.directory) { status = 0xc00000bb; }
        else if (frame[5] == 13) {
          if (frame[4] != 1) { status = 0xc0000004; }
          else {
            uint8_t value = 0;
            memory.Read(ctx, frame[3], &value, 1);
            runtime_.Checkpoint(ctx);
            for (auto& [handle, opened] : files_) {
              if (opened.temporary && opened.cache == selected.cache && opened.path == selected.path) { opened.deletePending = value != 0; }
            }
          }
        } else if (frame[4] != 16) { status = 0xc0000004; }
        else if (selected.deletePending) { status = 0xc0000056; }
        else {
          std::array<uint32_t, 4> rename;
          memory.Read(ctx, frame[3], rename.data(), sizeof(rename));
          std::string target;
          status = ReadName(ctx, frame[3] + 8, target);
          bool temporary = false, cache = false;
          if (!status && rename[1]) {
            const auto root = files_.find(rename[1]);
            if (root == files_.end()) { status = 0xc0000008; }
            else if (!root->second.directory) { status = 0xc0000024; }
            else if (target.front() == '\\' || target.find(':') != std::string::npos) { status = 0xc0000033; }
            else {
              temporary = root->second.temporary; cache = root->second.cache;
              target = root->second.path + (root->second.path.empty() ? "" : "\\") + target;
            }
          } else if (!status) {
            if (target.starts_with("\\??\\")) { target.erase(0, 4); }
            if (target.size() >= 2 && target[1] == ':') {
              const auto alias = aliases_.find(target.substr(0, 2));
              if (alias == aliases_.end()) { status = 0xc000003a; }
              else { target = alias->second + target.substr(2); }
            }
            if (!status && !ResolveDevice(target, temporary, &cache)) { status = 0xc000003a; }
          }
          if (!status && !ValidPath(target)) { status = 0xc0000033; }
          if (!status && (!temporary || cache != selected.cache)) { status = 0xc00000d4; }
          std::vector<std::pair<File*, std::string>> replacements;
          if (!status) {
            for (auto& [handle, opened] : files_) {
              if (!opened.temporary || opened.cache != selected.cache) { continue; }
              if (opened.path == selected.path) { replacements.emplace_back(&opened, target); }
              else if (opened.path == target) { status = 0xc0000043; break; }
            }
          }
          runtime_.Checkpoint(ctx);
          if (!status) { status = Storage(selected)->Rename(selected.path, target, (rename[0] & 0xff) != 0); }
          if (!status) { for (auto& [opened, path] : replacements) { opened->path.swap(path); } }
        }
        publish(status);
        return;
      }
      if (file == files_.end()) { status = 0xc0000008; }
      else if (frame[5] != 14 || file->second.raw) { status = 0xc00000bb; }
      else if (frame[4] != 8) { status = 0xc0000004; }
      else if (file->second.directory) { status = 0xc00000ba; }
      else if (file->second.alertable || (!file->second.synchronous && !file->second.reads && !file->second.writeData)) { status = 0xc00000bb; }
      else {
        memory.Read(ctx, frame[3], &position, sizeof(position));
        if (position >> 63) { status = 0xc000000d; }
      }
      if (!status && file->second.disc) { selected = file->second.disc; }
      else {
        runtime_.Checkpoint(ctx);
        if (!status) { file->second.position = position; }
        publish(status);
        return;
      }
    }
    std::unique_lock serial(selected->io);
    auto transition = memory.Mutate();
    RequireOwner(ctx, base);
    runtime_.Checkpoint(ctx);
    validate();
    std::lock_guard lock(lock_);
    const auto file = files_.find(frame[1]);
    const uint32_t status = file == files_.end() || file->second.disc != selected ? 0xc0000008 : 0;
    runtime_.Checkpoint(ctx);
    if (!status) {
      selected->position = position;
      file->second.position = position;
    }
    publish(status);
  }

  void Query(X86Context& ctx, uint8_t* base, bool volume) {
    RequireOwner(ctx, base);
    auto& memory = *image_.memory;
    std::array<uint32_t, 6> frame{};
    std::shared_ptr<DiscFile> selected;
    const auto overlaps = [](uint32_t first, uint32_t size, uint32_t second, uint32_t count) {
      return uint64_t(first) < uint64_t(second) + count && uint64_t(second) < uint64_t(first) + size;
    };
    const auto validate = [&] {
      const uint32_t statusOutput = frame[2], output = frame[3], length = frame[4];
      if (!output || !statusOutput || (output & 3) || (statusOutput & 3) ||
          overlaps(output, length, statusOutput, 8) || overlaps(output, length, ctx.esp, sizeof(frame)) ||
          overlaps(statusOutput, 8, ctx.esp, sizeof(frame))) {
        throw Fault(ctx, output, length, "invalid volume query output spans");
      }
      memory.CheckRange(ctx, output, length);
      memory.CheckRange(ctx, statusOutput, 8);
    };
    const auto publish = [&](uint32_t status, const void* bytes, uint32_t size) {
      ReportOperationLocked(frame[1], volume ? 218 : 211, frame[5], status, frame[4]);
      if (!status) { memory.Write(ctx, frame[3], bytes, size); }
      const std::array<uint32_t, 2> result = {status, status ? 0u : size};
      memory.Write(ctx, frame[2], result.data(), sizeof(result));
      ctx.eax = status;
      ctx.esp += sizeof(frame);
    };
    {
      auto transition = memory.Mutate();
      runtime_.Checkpoint(ctx);
      if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "volume query frame overflow"); }
      memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      validate();
      const uint32_t handle = frame[1], length = frame[4], information = frame[5];
      const bool position = !volume && information == 14;
      std::lock_guard lock(lock_);
      uint32_t status = 0;
      const auto file = files_.find(handle);
      if (file == files_.end()) { status = 0xc0000008; }
      else if (file->second.raw || (position && file->second.directory) || (volume && !file->second.temporary) ||
          (!position && information != (volume ? 3u : 34u))) { status = 0xc00000bb; }
      const uint32_t outputSize = position ? 8 : volume ? 24 : 56;
      if (!status && length < outputSize) { status = 0xc0000004; }
      if (!status && position && !file->second.temporary) {
        selected = file->second.disc;
        if (!selected || !selected->entry) { throw Fault(ctx, handle, 0, "disc position lost its owned source"); }
      } else {
        std::array<uint32_t, 14> outputBytes{};
        if (!status && position) {
          outputBytes[0] = uint32_t(file->second.position);
          outputBytes[1] = uint32_t(file->second.position >> 32);
        }
        if (!status && volume) {
          const auto usage = Storage(file->second)->Usage();
          outputBytes[0] = uint32_t(usage.capacity / TemporaryFilesystem::AllocationUnit);
          outputBytes[2] = uint32_t((usage.capacity - usage.allocated) / TemporaryFilesystem::AllocationUnit);
          outputBytes[4] = TemporaryFilesystem::AllocationUnit / 512;
          outputBytes[5] = 512;
        }
        if (!status && !volume && !position && !file->second.temporary) {
          const auto& source = file->second.disc;
          if (!source || !source->entry) { throw Fault(ctx, handle, 0, "disc metadata lost its owned source"); }
          const uint64_t size = source->entry->dir ? 0 : source->entry->size;
          const uint64_t allocation = ((size + 4095) / 4096) * 4096;
          outputBytes[8] = uint32_t(allocation);
          outputBytes[9] = uint32_t(allocation >> 32);
          outputBytes[10] = uint32_t(size);
          outputBytes[11] = uint32_t(size >> 32);
          outputBytes[12] = source->entry->dir ? 0x11 : 1;
        }
        if (!status && !volume && !position && file->second.temporary) {
          const auto metadata = Storage(file->second)->Stat(file->second.path);
          if (!metadata) { status = 0xc0000034; }
          else {
            const auto put = [&](uint32_t index, uint64_t value) { outputBytes[index] = uint32_t(value); outputBytes[index + 1] = uint32_t(value >> 32); };
            put(0, metadata->creationTime);
            put(2, metadata->lastAccessTime);
            put(4, metadata->lastWriteTime);
            put(6, metadata->changeTime);
            put(8, metadata->directory ? TemporaryFilesystem::AllocationUnit : metadata->allocationSize);
            put(10, metadata->size);
            outputBytes[12] = metadata->attributes;
          }
        }
        runtime_.Checkpoint(ctx);
        publish(status, outputBytes.data(), outputSize);
        return;
      }
    }
    std::unique_lock serial(selected->io);
    auto transition = memory.Mutate();
    RequireOwner(ctx, base);
    runtime_.Checkpoint(ctx);
    validate();
    std::lock_guard lock(lock_);
    const auto file = files_.find(frame[1]);
    const uint32_t status = file == files_.end() || file->second.disc != selected ? 0xc0000008 : 0;
    runtime_.Checkpoint(ctx);
    publish(status, &selected->position, sizeof(selected->position));
  }
};
}

std::function<bool(uint32_t)> InstallFileProvider(Runtime& runtime, PreparedImage& image,
    std::shared_ptr<const kernel::DiscIndex> disc, std::shared_ptr<TemporaryFilesystem> temporary, std::vector<KernelDataExport>* dataExports,
    std::function<DiscReadSnapshot()>* discReads, std::function<void(FileOpenDiagnostic)> fileOpenDiagnostic,
    std::function<bool(uint32_t)>* ownsHandle, std::shared_ptr<RawCache> cache,
    std::function<void(FileOperationDiagnostic)> operationDiagnostic, uint32_t maximumReadBytes, uint32_t maximumHandles) {
  runtime.Checkpoint({});
  if (!image.memory || image.importsBound || runtime.ActiveWorkers() || !image.diagnosticImports.empty() || image.boundRuntime || image.diagnosticRuntime || runtime.KernelTarget(202) ||
      (dataExports && std::any_of(dataExports->begin(), dataExports->end(), [](const auto& entry) { return entry.ordinal == 40 || entry.ordinal == 326; }))) {
    throw Fault({}, 202, 0, "file provider requires an unpublished stopped image");
  }
  auto provider = std::make_shared<FileProvider>(runtime, image, std::move(disc), std::move(temporary), std::move(fileOpenDiagnostic), std::move(cache), std::move(operationDiagnostic), maximumReadBytes, maximumHandles);
  if (dataExports) { provider->InstallData(*dataExports); }
  runtime.RegisterKernel(67, [provider](X86Context& ctx, uint8_t* base) { provider->CreateAlias(ctx, base); });
  runtime.RegisterKernel(69, [provider](X86Context& ctx, uint8_t* base) { provider->DeleteAlias(ctx, base); });
  runtime.RegisterKernel(190, [provider](X86Context& ctx, uint8_t* base) { provider->Open(ctx, base, true); });
  runtime.RegisterKernel(196, [provider](X86Context& ctx, uint8_t* base) { provider->DeviceControl(ctx, base); });
  runtime.RegisterKernel(200, [provider](X86Context& ctx, uint8_t* base) { provider->DeviceControl(ctx, base, true); });
  runtime.RegisterKernel(202, [provider](X86Context& ctx, uint8_t* base) { provider->Open(ctx, base); });
  runtime.RegisterKernel(207, [provider](X86Context& ctx, uint8_t* base) { provider->QueryDirectory(ctx, base); });
  runtime.RegisterKernel(219, [provider](X86Context& ctx, uint8_t* base) { provider->Transfer(ctx, base, true); });
  runtime.RegisterKernel(236, [provider](X86Context& ctx, uint8_t* base) { provider->Transfer(ctx, base); });
  runtime.RegisterKernel(203, [provider](X86Context& ctx, uint8_t* base) { provider->OpenLink(ctx, base); });
  runtime.RegisterKernel(211, [provider](X86Context& ctx, uint8_t* base) { provider->Query(ctx, base, false); });
  runtime.RegisterKernel(215, [provider](X86Context& ctx, uint8_t* base) { provider->QueryLink(ctx, base); });
  runtime.RegisterKernel(218, [provider](X86Context& ctx, uint8_t* base) { provider->Query(ctx, base, true); });
  runtime.RegisterKernel(226, [provider](X86Context& ctx, uint8_t* base) { provider->SetInformation(ctx, base); });
  if (discReads) { *discReads = [provider] { return provider->DiscReads(); }; }
  if (ownsHandle) { *ownsHandle = [provider](uint32_t handle) { return provider->Contains(handle); }; }
  return [provider](uint32_t handle) { return provider->Close(handle); };
}

}
