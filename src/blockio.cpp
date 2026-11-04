// blockio.cpp — POSIX implementation (Linux: O_DIRECT, block ioctls; macOS:
// F_NOCACHE, disk ioctls). Windows: blockio_windows.cpp.
#include "blockio.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/fs.h>
#endif
#ifdef __APPLE__
#include <sys/disk.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace kiln {

#ifdef __linux__
namespace {
constexpr uint32_t kFileDirectAlign = 4096;  // safe O_DIRECT alignment for regular files
}
#endif

Fd::Fd(NativeFd fd) : fd_(fd < 0 ? -1 : fd) {}

Fd::~Fd() {
    if (fd_ >= 0) ::close(fd_);
}

bool Fd::valid() const { return fd_ >= 0; }

Fd::Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}

Fd& Fd::operator=(Fd&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
}

AlignedBuffer::AlignedBuffer(size_t n)
    : p_(static_cast<unsigned char*>(std::aligned_alloc(4096, round_up(n, 4096)))), n_(n) {
    if (!p_) throw std::bad_alloc();
}

AlignedBuffer::~AlignedBuffer() { std::free(p_); }

void throw_errno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

void pwrite_all(NativeFd fd, const void* data, size_t n, uint64_t off, const std::string& name) {
    auto p = static_cast<const unsigned char*>(data);
    while (n > 0) {
        const ssize_t w = ::pwrite(fd, p, n, static_cast<off_t>(off));
        if (w < 0) {
            if (errno == EINTR) continue;
            throw_errno("write to " + name + " at byte " + std::to_string(off));
        }
        if (w == 0) throw std::runtime_error("write to " + name + " stalled at byte " + std::to_string(off));
        p += w;
        n -= static_cast<size_t>(w);
        off += static_cast<uint64_t>(w);
    }
}

size_t pread_all(NativeFd fd, void* data, size_t n, uint64_t off, const std::string& name, bool direct) {
    auto p = static_cast<unsigned char*>(data);
    size_t got = 0;
    while (got < n) {
        const ssize_t r = ::pread(fd, p + got, n - got, static_cast<off_t>(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw_errno("read from " + name + " at byte " + std::to_string(off + got));
        }
        if (r == 0) break;
        got += static_cast<size_t>(r);
        if (direct && got < n) break;
    }
    return got;
}

Fd open_file(const std::filesystem::path& path, Access access, bool truncate) {
    int flags = O_CLOEXEC;
    if (access == Access::read)
        flags |= O_RDONLY;
    else
        flags |= (access == Access::write ? O_WRONLY : O_RDWR) | O_CREAT | (truncate ? O_TRUNC : 0);
    Fd fd(::open(path.c_str(), flags, 0644));
    if (!fd.valid()) throw_errno("open " + path.string());
    return fd;
}

uint64_t file_size(NativeFd fd, const std::string& name) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) throw_errno("stat " + name);
    return static_cast<uint64_t>(st.st_size);
}

void truncate_file(NativeFd fd, uint64_t size, const std::string& name) {
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) throw_errno("truncate " + name);
}

bool same_file(NativeFd fd, const TargetSpec& other) {
    struct stat a {}, b {};
    if (::fstat(fd, &a) != 0) return false;
    if ((other.has_fd ? ::fstat(other.fd, &b) : ::stat(other.path.c_str(), &b)) != 0) return false;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

namespace {

bool is_device(const struct stat& st) {
#ifdef __APPLE__
    return S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode);  // /dev/diskN, /dev/rdiskN (raw)
#else
    return S_ISBLK(st.st_mode);
#endif
}

// Size, sector and alignment of an open drive or file.
void query(Target& t) {
    const int fd = t.fd.get();
    if (t.block) {
#ifdef __APPLE__
        uint32_t sector = 512;
        uint64_t count = 0;
        if (::ioctl(fd, DKIOCGETBLOCKSIZE, &sector) != 0 || ::ioctl(fd, DKIOCGETBLOCKCOUNT, &count) != 0)
            throw_errno("size of " + t.name);
        t.size = count * sector;
        t.sector = sector < 512 ? 512 : sector;
        t.align = t.sector;  // a raw disk takes whole sectors only, cached or not
#else
        uint64_t bytes = 0;
        if (::ioctl(fd, BLKGETSIZE64, &bytes) != 0) throw_errno("size of " + t.name);
        t.size = bytes;
        int sector = 512;
        if (::ioctl(fd, BLKSSZGET, &sector) != 0 || sector < 512) sector = 512;
        t.sector = static_cast<uint32_t>(sector);
        t.align = t.direct ? t.sector : 1;
#endif
    } else {
        struct stat fst {};
        if (::fstat(fd, &fst) != 0) throw_errno("stat " + t.name);
        t.size = static_cast<uint64_t>(fst.st_size);
#ifdef __APPLE__
        t.align = 1;  // F_NOCACHE has no alignment rules on files
#else
        t.align = t.direct ? kFileDirectAlign : 1;
#endif
    }
}

}  // namespace

Target open_target(const TargetSpec& spec, Access access, bool direct_on_files, bool create) {
    Target t;
    t.name = spec.path.string();
    const bool writing = access != Access::read;

    if (spec.has_fd) {  // handed over (macOS authopen): check what it is, keep our own dup
        struct stat st {};
        if (::fstat(spec.fd, &st) != 0) throw_errno("stat " + t.name);
        if (!is_device(st) && !S_ISREG(st.st_mode)) throw std::runtime_error(t.name + " is neither a drive nor a file");
        t.block = is_device(st);
        const int fd = ::fcntl(spec.fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) throw_errno("dup " + t.name);
        t.fd = Fd(fd);
#ifdef __APPLE__
        t.direct = ::fcntl(fd, F_NOCACHE, 1) == 0;
#endif
        query(t);
        return t;
    }

    const std::filesystem::path& path = spec.path;
    struct stat st {};
    const bool exists = ::stat(path.c_str(), &st) == 0;
    if (!exists && errno != ENOENT) throw_errno("stat " + t.name);
    if (!exists && (!writing || !create)) throw std::runtime_error(t.name + " does not exist");
    if (exists && is_device(st))
        t.block = true;
    else if (exists && !S_ISREG(st.st_mode))
        throw std::runtime_error(t.name + " is neither a drive nor a regular file");

    int flags = O_CLOEXEC;
    flags |= access == Access::read ? O_RDONLY : access == Access::write ? O_WRONLY : O_RDWR;
#ifdef __linux__
    if (writing && t.block) flags |= O_EXCL;  // EBUSY while any partition on it is mounted
#endif
    if (writing && !t.block && create) flags |= O_CREAT;
    const bool try_direct = t.block || direct_on_files;
    int fd = -1;
#ifdef __linux__
    fd = try_direct ? ::open(path.c_str(), flags | O_DIRECT, 0644) : -1;
    if (fd >= 0)
        t.direct = true;
    else if (!try_direct || errno == EINVAL)  // tmpfs and friends: no O_DIRECT
        fd = ::open(path.c_str(), flags, 0644);
#else
    fd = ::open(path.c_str(), flags, 0644);
    if (fd >= 0 && try_direct) t.direct = ::fcntl(fd, F_NOCACHE, 1) == 0;
#endif
    if (fd < 0) {
        if (errno == EBUSY) throw std::runtime_error(t.name + " is busy: a partition on it is still mounted or in use");
        if (errno == EACCES || errno == EPERM)
            throw std::runtime_error("no permission to open " + t.name +
                                     (t.block ? " (writing to a drive needs root)" : ""));
        throw_errno("open " + t.name);
    }
    t.fd = Fd(fd);
    query(t);
    return t;
}

void sync_fd(NativeFd fd, const std::string& name) {
#ifdef __APPLE__
    // fsync() on macOS stops at the drive's cache; F_FULLFSYNC asks the drive to
    // flush it (not every device supports it: fall back to fsync).
    if (::fcntl(fd, F_FULLFSYNC) == 0) return;
    if (::fsync(fd) != 0) throw_errno("flush " + name);
#else
    if (::fdatasync(fd) != 0) throw_errno("flush " + name);
#endif
}

void advise_sequential(NativeFd fd) {
#ifdef __linux__
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
    ::fcntl(fd, F_RDAHEAD, 1);
#endif
}

void drop_cached(const Target& t) {
    if (t.direct) return;
#ifdef __linux__
    if (t.block)
        ::ioctl(t.fd.get(), BLKFLSBUF, 0);  // needs root, which writing a drive already has
    else
        ::posix_fadvise(t.fd.get(), 0, 0, POSIX_FADV_DONTNEED);
#endif
}

bool reread_partition_table(const TargetSpec& device) {
#ifdef __linux__
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (attempt) ::usleep(500 * 1000);
        Fd fd(device.has_fd ? ::fcntl(device.fd, F_DUPFD_CLOEXEC, 0) : ::open(device.path.c_str(), O_RDONLY | O_CLOEXEC));
        if (!fd.valid()) return false;
        if (::ioctl(fd.get(), BLKRRPART, 0) == 0) return true;
        if (errno != EBUSY) return false;
    }
    return false;
#else
    (void)device;
    return true;
#endif
}

}  // namespace kiln
