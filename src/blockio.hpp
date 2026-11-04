// blockio.hpp — raw access to a drive or an image file, shared by the raw
// writer and the Windows builder.
//
// A target is a block device or a regular file. Drives are opened exclusively
// (Linux O_EXCL: EBUSY while anything on them is mounted; Windows: every volume
// on the disk locked and dismounted for as long as the target is open) and
// uncached where the OS allows it, so reads see the medium rather than memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace kiln {

// What the OS hands out for an open file or drive: a descriptor, or a HANDLE.
#ifdef _WIN32
using NativeFd = void*;
#else
using NativeFd = int;
#endif

class Fd {
public:
    Fd() = default;
    explicit Fd(NativeFd fd);  // takes ownership; an invalid value (-1, INVALID_HANDLE_VALUE) stays invalid
    ~Fd();
    Fd(Fd&& o) noexcept;
    Fd& operator=(Fd&& o) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    NativeFd get() const { return fd_; }
    bool valid() const;

private:
#ifdef _WIN32
    NativeFd fd_ = nullptr;
#else
    NativeFd fd_ = -1;
#endif
};

// Uncached I/O wants buffer, offset and length aligned; 4096 covers every
// logical block size a stick or card reports.
class AlignedBuffer {
public:
    explicit AlignedBuffer(size_t n);
    ~AlignedBuffer();
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    unsigned char* get() const { return p_; }
    size_t size() const { return n_; }

private:
    unsigned char* p_;
    size_t n_;
};

// Throws for the last OS error (errno; GetLastError on Windows), prefixed by `what`.
[[noreturn]] void throw_errno(const std::string& what);

inline uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
inline uint64_t round_down(uint64_t v, uint64_t a) { return v / a * a; }

void pwrite_all(NativeFd fd, const void* p, size_t n, uint64_t off, const std::string& name);
// Reads up to n bytes; fewer only at end of file. With uncached I/O a short read
// IS the end (a follow-up read at the unaligned offset would fail).
size_t pread_all(NativeFd fd, void* p, size_t n, uint64_t off, const std::string& name, bool direct);

enum class Access { read, write, read_write };

// A regular file, cached: read, or written (created if missing, truncated when
// `truncate`). Throws on failure.
Fd open_file(const std::filesystem::path& path, Access access, bool truncate = false);
uint64_t file_size(NativeFd fd, const std::string& name);
void truncate_file(NativeFd fd, uint64_t size, const std::string& name);

struct Target {
    Fd fd;
    std::string name;
    bool block = false;
    bool direct = false;
    uint64_t size = 0;      // device size, or the file's current length
    uint32_t sector = 512;  // logical sector (files: 512)
    uint32_t align = 1;     // I/O granularity: the sector with uncached I/O, else 1
    std::shared_ptr<void> held;  // Windows: the volume locks, released with the target
};

// Where to write: a path, or a drive already opened for us (macOS: the
// descriptor authopen hands over; Windows: the handle the elevated helper
// opened). Implicit from a path, so most callers just pass one.
struct TargetSpec {
    TargetSpec(const std::filesystem::path& p) : path(p) {}
    TargetSpec(const std::string& p) : path(p) {}
    TargetSpec(const char* p) : path(p) {}
    static TargetSpec descriptor(NativeFd fd, const std::string& name) {
        TargetSpec s(name);
        s.fd = fd;
        s.has_fd = true;
        return s;
    }
    std::filesystem::path path;  // with fd: only the name in messages
    NativeFd fd{};               // borrowed: open_target duplicates it, the owner closes it
    bool has_fd = false;
};

// `direct_on_files`: also bypass the cache on regular files (4096-byte
// alignment). `create`: a writing open creates a missing regular file.
Target open_target(const TargetSpec& spec, Access access, bool direct_on_files = true, bool create = true);

// True when `fd` (an open regular file) and `other` are the same file.
bool same_file(NativeFd fd, const TargetSpec& other);

// Data and metadata of `fd` on stable storage (fdatasync; F_FULLFSYNC on macOS;
// FlushFileBuffers on Windows).
void sync_fd(NativeFd fd, const std::string& name);
// We will read `fd` front to back (posix_fadvise where there is one).
void advise_sequential(NativeFd fd);

#ifdef __APPLE__
// macOS: the drive opened read/write through /usr/libexec/authopen, which asks
// for an administrator's approval (its own dialog) and hands back the
// descriptor; Kiln itself never runs as root. -1 with `error` set on refusal.
int authopen_device(const std::string& path, std::string* error);
#endif

#ifdef _WIN32
// True when this process runs elevated (an administrator's full token).
bool running_as_admin();
#endif

// After writing: make the next read of `t` come from the medium (no-op with uncached I/O).
void drop_cached(const Target& t);

// Makes the OS pick up the new partition table. Linux retries while it answers
// EBUSY (a desktop's device notifier can still hold an old partition for a
// moment); false when it never took it, and the drive must be replugged.
// macOS re-reads a disk on its own when it is ejected: always true there.
// Windows: IOCTL_DISK_UPDATE_PROPERTIES.
bool reread_partition_table(const TargetSpec& device);

}  // namespace kiln
