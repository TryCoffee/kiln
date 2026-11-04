// blockio_windows.cpp — Windows implementation (Win32 file and disk I/O).
//
// A drive is "\\.\PhysicalDriveN", opened FILE_FLAG_NO_BUFFERING |
// FILE_FLAG_WRITE_THROUGH (sector-aligned buffers, offsets and lengths). Windows
// refuses writes into a mounted volume's sectors, so a writing open first locks
// and dismounts every volume on the disk and keeps those locks (Target::held)
// until the target is closed: the counterpart of Linux's O_EXCL.
#include "blockio.hpp"

#include "win32_util.hpp"

#include <windows.h>
#include <winioctl.h>
#include <malloc.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace kiln {
namespace {

constexpr uint32_t kFileDirectAlign = 4096;  // a multiple of every volume's sector size

bool is_device_path(const std::string& p) { return p.rfind("\\\\.\\", 0) == 0 || p.rfind("//./", 0) == 0; }

[[noreturn]] void fail_open(const std::string& name, bool block) {
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) throw std::runtime_error(name + " does not exist");
    if (e == ERROR_ACCESS_DENIED)
        throw std::runtime_error("no permission to open " + name + (block ? " (writing to a drive needs an administrator)" : ""));
    if (e == ERROR_SHARING_VIOLATION) throw std::runtime_error(name + " is in use by another program");
    throw std::runtime_error("open " + name + ": " + win::error_text(e));
}

// Locks and dismounts every volume on the disk behind `disk`; the locks hold
// for as long as the returned object lives.
std::shared_ptr<void> lock_volumes(HANDLE disk, const std::string& name) {
    const auto number = win::disk_number(disk);
    if (!number) throw std::runtime_error("cannot tell which disk " + name + " is");
    auto held = std::make_shared<std::vector<Fd>>();
    for (const win::Volume& v : win::list_volumes())
        for (DWORD d : v.disks)
            if (d == *number) {
                held->push_back(win::lock_and_dismount(v));
                break;
            }
    return held;
}

void query(Target& t) {
    HANDLE h = t.fd.get();
    if (t.block) {
        DISK_GEOMETRY_EX g{};
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &g, sizeof g, &got, nullptr))
            throw_errno("size of " + t.name);
        t.size = static_cast<uint64_t>(g.DiskSize.QuadPart);
        t.sector = g.Geometry.BytesPerSector < 512 ? 512 : g.Geometry.BytesPerSector;
        t.align = t.sector;  // NO_BUFFERING: whole sectors only
    } else {
        t.size = file_size(h, t.name);
        t.align = t.direct ? kFileDirectAlign : 1;
    }
}

}  // namespace

Fd::Fd(NativeFd fd) : fd_(fd == INVALID_HANDLE_VALUE ? nullptr : fd) {}

Fd::~Fd() {
    if (fd_) CloseHandle(fd_);
}

bool Fd::valid() const { return fd_ != nullptr; }

Fd::Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, nullptr)) {}

Fd& Fd::operator=(Fd&& o) noexcept {
    if (this != &o) {
        if (fd_) CloseHandle(fd_);
        fd_ = std::exchange(o.fd_, nullptr);
    }
    return *this;
}

AlignedBuffer::AlignedBuffer(size_t n)
    : p_(static_cast<unsigned char*>(_aligned_malloc(round_up(n, 4096), 4096))), n_(n) {
    if (!p_) throw std::bad_alloc();
}

AlignedBuffer::~AlignedBuffer() { _aligned_free(p_); }

void throw_errno(const std::string& what) { throw std::runtime_error(what + ": " + win::error_text(GetLastError())); }

void pwrite_all(NativeFd fd, const void* data, size_t n, uint64_t off, const std::string& name) {
    auto p = static_cast<const unsigned char*>(data);
    while (n > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 1u << 30));
        OVERLAPPED ov{};  // on a synchronous handle: just the position
        ov.Offset = static_cast<DWORD>(off);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD w = 0;
        if (!WriteFile(fd, p, chunk, &w, &ov)) throw_errno("write to " + name + " at byte " + std::to_string(off));
        if (w == 0) throw std::runtime_error("write to " + name + " stalled at byte " + std::to_string(off));
        p += w;
        n -= w;
        off += w;
    }
}

size_t pread_all(NativeFd fd, void* data, size_t n, uint64_t off, const std::string& name, bool direct) {
    auto p = static_cast<unsigned char*>(data);
    size_t got = 0;
    while (got < n) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n - got, 1u << 30));
        OVERLAPPED ov{};
        const uint64_t at = off + got;
        ov.Offset = static_cast<DWORD>(at);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD r = 0;
        if (!ReadFile(fd, p + got, chunk, &r, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            throw_errno("read from " + name + " at byte " + std::to_string(at));
        }
        if (r == 0) break;
        got += r;
        if (direct && got < n) break;
    }
    return got;
}

Fd open_file(const std::filesystem::path& path, Access access, bool truncate) {
    const DWORD rights = access == Access::read ? GENERIC_READ : access == Access::write ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    const DWORD disposition = access == Access::read ? OPEN_EXISTING : truncate ? CREATE_ALWAYS : OPEN_ALWAYS;
    Fd fd(CreateFileW(path.c_str(), rights, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                      FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!fd.valid()) fail_open(path.string(), false);
    return fd;
}

uint64_t file_size(NativeFd fd, const std::string& name) {
    LARGE_INTEGER s{};
    if (!GetFileSizeEx(fd, &s)) throw_errno("size of " + name);
    return static_cast<uint64_t>(s.QuadPart);
}

void truncate_file(NativeFd fd, uint64_t size, const std::string& name) {
    FILE_END_OF_FILE_INFO eof{};
    eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(fd, FileEndOfFileInfo, &eof, sizeof eof)) throw_errno("truncate " + name);
}

bool same_file(NativeFd fd, const TargetSpec& other) {
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    if (!GetFileInformationByHandle(fd, &a)) return false;
    Fd own;
    HANDLE h = other.fd;
    if (!other.has_fd) {
        own = Fd(CreateFileW(other.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!own.valid()) return false;
        h = own.get();
    }
    if (!GetFileInformationByHandle(h, &b)) return false;  // a drive: never the image file
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
           a.nFileIndexLow == b.nFileIndexLow;
}

Target open_target(const TargetSpec& spec, Access access, bool direct_on_files, bool create) {
    Target t;
    t.name = spec.path.string();
    const bool writing = access != Access::read;
    const DWORD rights = access == Access::read ? GENERIC_READ : access == Access::write ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;

    if (spec.has_fd) {  // handed over (the elevated helper's drive): ours to duplicate, its locks stay with the owner
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), spec.fd, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            throw_errno("duplicate the handle of " + t.name);
        t.fd = Fd(dup);
        t.block = win::disk_number(dup).has_value();
        t.direct = t.block;  // Kiln opens drives unbuffered, always
        query(t);
        return t;
    }

    if (is_device_path(t.name)) {
        t.block = true;
        t.direct = true;
        t.fd = Fd(CreateFileW(spec.path.c_str(), rights, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (!t.fd.valid()) fail_open(t.name, true);
        if (writing) t.held = lock_volumes(t.fd.get(), t.name);
        query(t);
        return t;
    }

    std::error_code ec;
    const bool exists = std::filesystem::exists(spec.path, ec);
    if (!exists && (!writing || !create)) throw std::runtime_error(t.name + " does not exist");
    if (exists && !std::filesystem::is_regular_file(spec.path, ec))
        throw std::runtime_error(t.name + " is neither a drive nor a regular file");
    t.direct = direct_on_files;
    const DWORD flags = t.direct ? FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH : FILE_ATTRIBUTE_NORMAL;
    t.fd = Fd(CreateFileW(spec.path.c_str(), rights, FILE_SHARE_READ | (writing ? 0 : FILE_SHARE_WRITE), nullptr,
                          writing && create ? OPEN_ALWAYS : OPEN_EXISTING, flags, nullptr));
    if (!t.fd.valid()) fail_open(t.name, false);
    query(t);
    return t;
}

void sync_fd(NativeFd fd, const std::string& name) {
    if (!FlushFileBuffers(fd)) throw_errno("flush " + name);
}

void advise_sequential(NativeFd) {}

void drop_cached(const Target&) {}  // drives and direct files are unbuffered; nothing to drop

bool running_as_admin() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD got = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &got) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

bool reread_partition_table(const TargetSpec& device) {
    Fd fd;
    if (device.has_fd) {
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), device.fd, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return false;
        fd = Fd(dup);
    } else {
        fd = Fd(CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr));
    }
    if (!fd.valid()) return false;
    DWORD got = 0;
    return DeviceIoControl(fd.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr) != 0;
}

}  // namespace kiln
