#include "kernel/disc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace kernel {
namespace {

std::unique_ptr<Disc> mounted;

}

Disc::~Disc() {
    if (file_) {
        CloseHandle(file_);
    }
}

uint32_t Disc::ReadAt(uint64_t at, void* out, uint32_t len) const {
    thread_local HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED operation{};
    operation.Offset = DWORD(at);
    operation.OffsetHigh = DWORD(at >> 32);
    operation.hEvent = event;
    DWORD got = 0;
    if (!ReadFile(file_, out, len, nullptr, &operation) && GetLastError() != ERROR_IO_PENDING) {
        return 0;
    }
    if (!GetOverlappedResult(file_, &operation, &got, TRUE)) {
        return 0;
    }
    return got;
}

bool Disc::Open(const std::filesystem::path& path, std::string& error) {
    index_ = DiscIndex{};
    path_.clear();
    if (file_) {
        CloseHandle(file_);
    }
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        file_ = nullptr;
        error = "Cannot open the disc file.";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size) || size.QuadPart < 0) {
        error = "Cannot read the disc file size.";
        CloseHandle(file_);
        file_ = nullptr;
        return false;
    }
    if (!index_.Open(uint64_t(size.QuadPart), [this](uint64_t at, void* out, uint32_t length) {
            return ReadAt(at, out, length);
        }, error)) {
        CloseHandle(file_);
        file_ = nullptr;
        return false;
    }
    path_ = path;
    return true;
}

const Disc::Entry* Disc::Find(const std::vector<std::string>& parts) const {
    return index_.Find(parts);
}

const Disc::Entry* Disc::Find(std::string_view path) const {
    return index_.Find(path);
}

uint32_t Disc::Read(const Entry& file, uint64_t pos, void* out, uint32_t len) const {
    return index_.Read(file, pos, out, len);
}

bool Disc::Load(std::string_view path, std::vector<uint8_t>& out) const {
    return index_.Load(path, out);
}

void MountDisc(std::unique_ptr<Disc> disc) { mounted = std::move(disc); }

const Disc* MountedDisc() { return mounted.get(); }

}
