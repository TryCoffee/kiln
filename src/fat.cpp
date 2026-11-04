// fat.cpp — FatFs on top of a Target region (or a memory image), plus FatFs's disk I/O glue.
#include "fat.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

extern "C" {
#include "ff.h"
#include "diskio.h"
}

namespace kiln {
namespace {

constexpr size_t kBounce = 4u << 20;
constexpr size_t kChunk = 4u << 20;
constexpr size_t kCacheBytes = 64u << 20;

// The region FatFs's drive 0 is bound to while a format or a FatVolume runs:
// part of a target, or an image in memory (`mem`).
struct Region {
    unsigned char* mem = nullptr;
    NativeFd fd{};
    std::string name;
    uint64_t offset = 0;
    uint64_t sectors = 0;
    uint32_t sector = 512;
    bool direct = false;   // short reads mean end of device (O_DIRECT semantics)
    bool aligned = false;  // I/O must use aligned buffers: go through `bounce`
    uint32_t align = 1;    // the target's I/O granularity (Target::align)
    AlignedBuffer bounce{kBounce};
    // FatFs's single-sector I/O (the FAT and the directories), written through:
    // FatFs reads a directory one sector at a time and scans all of it to create
    // a file, so without this every file costs a pass over its directory on the
    // medium. Per mount: a fresh mount (verify) reads each sector from the medium.
    std::map<uint64_t, std::vector<unsigned char>> cache;
};

// Drops cached sectors in [first, first + count): about to be written another way.
void forget(Region& r, uint64_t first, uint64_t count) {
    r.cache.erase(r.cache.lower_bound(first), r.cache.lower_bound(first + count));
}

void remember(Region& r, uint64_t sector, const unsigned char* data) {
    if (r.cache.size() >= kCacheBytes / r.sector) r.cache.clear();
    r.cache[sector].assign(data, data + r.sector);
}

Region* g_region = nullptr;

class Bind {
public:
    Bind(Region& r) {
        if (g_region) throw std::logic_error("only one FAT volume at a time");
        g_region = &r;
    }
    ~Bind() { g_region = nullptr; }
};

void bind_target(Region& r, Target& t, uint64_t offset, uint64_t length) {
    if (offset % t.sector || length % t.sector) throw std::invalid_argument("FAT region not sector-aligned");
    r.fd = t.fd.get();
    r.name = t.name;
    r.offset = offset;
    r.sector = t.sector;
    r.sectors = length / t.sector;
    r.direct = t.direct;
    r.aligned = t.align > 1;
    r.align = t.align;
}

void bind_memory(Region& r, unsigned char* image, size_t bytes) {
    if (bytes % 512) throw std::invalid_argument("FAT image not a whole number of sectors");
    r.mem = image;
    r.name = "the FAT image in memory";
    r.sector = 512;
    r.sectors = bytes / 512;
}

const char* fresult_text(FRESULT r) {
    switch (r) {
        case FR_OK: return "ok";
        case FR_DISK_ERR: return "disk I/O error";
        case FR_INT_ERR: return "internal FatFs error";
        case FR_NOT_READY: return "drive not ready";
        case FR_NO_FILE: return "no such file";
        case FR_NO_PATH: return "no such directory";
        case FR_INVALID_NAME: return "invalid name";
        case FR_DENIED: return "denied (directory full or no space?)";
        case FR_EXIST: return "already exists";
        case FR_INVALID_OBJECT: return "invalid object";
        case FR_WRITE_PROTECTED: return "write protected";
        case FR_INVALID_DRIVE: return "invalid drive";
        case FR_NOT_ENABLED: return "volume not mounted";
        case FR_NO_FILESYSTEM: return "no FAT filesystem";
        case FR_MKFS_ABORTED: return "format aborted (unsuitable size?)";
        case FR_TIMEOUT: return "timeout";
        case FR_LOCKED: return "locked";
        case FR_NOT_ENOUGH_CORE: return "out of memory";
        case FR_TOO_MANY_OPEN_FILES: return "too many open files";
        case FR_INVALID_PARAMETER: return "invalid parameter";
    }
    return "unknown FatFs error";
}

void check(FRESULT r, const std::string& what) {
    if (r != FR_OK) throw std::runtime_error(what + ": " + fresult_text(r));
}

// File data does not go through FatFs. f_write and f_read move at most one
// cluster per disk call, even across contiguous clusters: 32 KiB I/Os, about
// 10 MB/s on a USB stick against 100+ MB/s with 4 MiB ones. So a file's run of
// contiguous clusters is moved here in kChunk pieces, straight to its sectors,
// and FatFs keeps only the FAT and the directory entries.

// Where the run of `count` clusters from `first` starts on the target, or
// nothing when it cannot be moved that way: a cluster number out of range, or
// a cluster the target's I/O alignment rules do not allow whole pieces into.
std::optional<uint64_t> run_offset(const Region& r, const FATFS& fs, DWORD first, DWORD count) {
    if (r.mem) return std::nullopt;  // FatFs's own path: it is all memcpy there
    if (first < 2 || first >= fs.n_fatent || count > fs.n_fatent - first) return std::nullopt;
    const uint64_t cluster = uint64_t{fs.csize} * r.sector;
    const uint64_t at = r.offset + (uint64_t{fs.database} + uint64_t{fs.csize} * (first - 2)) * r.sector;
    if (at % r.align || cluster % r.align || kChunk % r.align) return std::nullopt;
    return at;
}

// Bytes of I/O that carry n bytes of data: whole sectors, aligned for the target.
size_t io_length(const Region& r, size_t n) { return static_cast<size_t>(round_up(n, std::max(r.sector, r.align))); }

// The file's cluster runs as (count, first cluster) pairs, from FatFs's
// fast-seek table: one pair for a contiguous file, none for an empty one.
std::vector<DWORD> cluster_runs(FIL& f, const std::string& path) {
    std::vector<DWORD> tbl(64);
    for (;;) {
        tbl[0] = static_cast<DWORD>(tbl.size());
        f.cltbl = tbl.data();
        const FRESULT r = f_lseek(&f, CREATE_LINKMAP);
        f.cltbl = nullptr;  // f_read keeps walking the FAT itself
        if (r != FR_NOT_ENOUGH_CORE) {
            check(r, "mapping the clusters of " + path);
            break;
        }
        tbl.resize(tbl[0]);  // the size it needs
    }
    // [0] = items used, then the pairs, then a terminating 0
    return std::vector<DWORD>(tbl.begin() + 1, tbl.begin() + tbl[0] - 1);
}

}  // namespace

struct FatVolume::Impl {
    Region region;
    std::unique_ptr<Bind> bind;
    FATFS fs{};
    AlignedBuffer buf{kChunk};
};

void FatVolume::format(Target& t, uint64_t offset, uint64_t length, const std::string& label, FatType type,
                       uint32_t cluster_bytes) {
    Region r;
    bind_target(r, t, offset, length);
    Bind bind(r);
    std::vector<unsigned char> work(1u << 20);
    const bool exfat = type == FatType::exfat;
    const char* name = exfat ? "exFAT" : "FAT32";
    // FM_SFD: the volume starts at sector 0 of the region, no MBR inside it.
    // align 0: data area aligned to GET_BLOCK_SIZE (1 MiB). FAT32: 2 FATs, as
    // Windows does; exFAT has one.
    MKFS_PARM opt{static_cast<BYTE>((exfat ? FM_EXFAT : FM_FAT32) | FM_SFD), static_cast<BYTE>(exfat ? 1 : 2), 0, 0,
                  cluster_bytes};
    check(f_mkfs("", &opt, work.data(), static_cast<UINT>(work.size())), std::string("formatting ") + name);
    FATFS fs{};
    check(f_mount(&fs, "", 1), std::string("mounting the new ") + name + " volume");
    std::string l = label.substr(0, 11);
    if (!l.empty()) check(f_setlabel(l.c_str()), "setting the volume label");
    check(f_mount(nullptr, "", 0), "unmounting");
    if (exfat) {  // the label lives in the root directory only
        sync_fd(t.fd.get(), t.name);
        return;
    }

    // f_setlabel() writes only the label entry in the root directory; the boot
    // sector keeps "NO NAME" (fsck.fat reports the mismatch). Write it into the
    // FAT32 boot sector (BS_VolLab, offset 71) and its backup copy, as mkfs.fat does.
    l.resize(11, ' ');
    AlignedBuffer s(t.sector);
    auto patch = [&](uint64_t sector_index) {
        const uint64_t at = offset + sector_index * t.sector;
        if (pread_all(t.fd.get(), s.get(), t.sector, at, t.name, t.direct) != t.sector)
            throw std::runtime_error("reading the new boot sector on " + t.name);
        std::memcpy(s.get() + 71, l.data(), 11);
        pwrite_all(t.fd.get(), s.get(), t.sector, at, t.name);
        return static_cast<uint64_t>(s.get()[50] | s.get()[51] << 8);  // BPB_BkBootSec
    };
    const uint64_t backup = patch(0);
    if (backup > 0 && backup < 32) patch(backup);
    sync_fd(t.fd.get(), t.name);
}

FatVolume::FatVolume(Target& t, uint64_t offset, uint64_t length) : impl_(std::make_unique<Impl>()) {
    bind_target(impl_->region, t, offset, length);
    impl_->bind = std::make_unique<Bind>(impl_->region);
    check(f_mount(&impl_->fs, "", 1), "mounting the FAT volume on " + t.name);
}

FatVolume::FatVolume(unsigned char* image, size_t bytes) : impl_(std::make_unique<Impl>()) {
    bind_memory(impl_->region, image, bytes);
    impl_->bind = std::make_unique<Bind>(impl_->region);
    check(f_mount(&impl_->fs, "", 1), "mounting " + impl_->region.name);
}

FatVolume::~FatVolume() {
    f_mount(nullptr, "", 0);
}

void FatVolume::make_dir(const std::string& path) {
    const FRESULT r = f_mkdir(path.c_str());
    if (r != FR_EXIST) check(r, "creating " + path);
}

bool FatVolume::exists(const std::string& path) {
    FILINFO info{};
    const FRESULT r = f_stat(path.c_str(), &info);
    if (r == FR_NO_FILE || r == FR_NO_PATH) return false;
    check(r, "looking up " + path);
    return true;
}

bool FatVolume::remove(const std::string& path) {
    const FRESULT r = f_unlink(path.c_str());
    if (r == FR_NO_FILE || r == FR_NO_PATH) return false;
    check(r, "deleting " + path);
    return true;
}

void FatVolume::write_file(const std::string& path, uint64_t size, const ReadFn& source, const ChunkFn& on_chunk) {
    if (!exfat() && size > 0xFFFFFFFFull) throw std::runtime_error(path + " is 4 GiB or larger: FAT32 cannot hold it");
    FIL f{};
    check(f_open(&f, path.c_str(), FA_WRITE | FA_CREATE_NEW), "creating " + path);
    // A write that fails or is cancelled leaves no file behind: allocated in
    // full up front, it would otherwise claim its whole size.
    struct Remover {
        FIL* f;
        const std::string& path;
        ~Remover() {
            if (!f) return;
            f_close(f);
            f_unlink(path.c_str());
        }
    } remover{&f, path};
    // Allocate one contiguous run and write it directly; when there is no such
    // run, FatFs allocates as it goes.
    std::optional<uint64_t> at;
    if (size > 0) {
        const FRESULT r = f_expand(&f, static_cast<FSIZE_t>(size), 1);
        if (r == FR_OK) {
            const uint64_t cluster = uint64_t{impl_->fs.csize} * impl_->region.sector;
            at = run_offset(impl_->region, impl_->fs, f.obj.sclust, static_cast<DWORD>((size + cluster - 1) / cluster));
        } else if (r != FR_DENIED) {
            check(r, "allocating " + path);
        }
    }
    for (uint64_t off = 0; at && off < size;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, size - off));
        source(impl_->buf.get(), n, off);
        const size_t io = io_length(impl_->region, n);
        std::memset(impl_->buf.get() + n, 0, io - n);  // the tail of the last cluster
        forget(impl_->region, (*at + off - impl_->region.offset) / impl_->region.sector, io / impl_->region.sector);
        pwrite_all(impl_->region.fd, impl_->buf.get(), io, *at + off, impl_->region.name);
        off += n;
        if (on_chunk) on_chunk(n);
    }
    for (uint64_t off = 0; !at && off < size;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, size - off));
        source(impl_->buf.get(), n, off);
        UINT written = 0;
        check(f_write(&f, impl_->buf.get(), static_cast<UINT>(n), &written), "writing " + path);
        if (written != n) throw std::runtime_error("writing " + path + ": the volume is full");
        off += n;
        if (on_chunk) on_chunk(n);
    }
    remover.f = nullptr;
    check(f_close(&f), "closing " + path);
}

uint64_t FatVolume::read_file(const std::string& path, const SinkFn& sink) {
    FIL f{};
    check(f_open(&f, path.c_str(), FA_READ), "opening " + path);
    struct Closer {
        FIL* f;
        ~Closer() {
            if (f) f_close(f);
        }
    } closer{&f};
    const uint64_t size = f.obj.objsize;
    const std::vector<DWORD> runs = cluster_runs(f, path);
    const uint64_t cluster = uint64_t{impl_->fs.csize} * impl_->region.sector;
    bool direct = true;
    for (size_t i = 0; i < runs.size(); i += 2) direct = direct && run_offset(impl_->region, impl_->fs, runs[i + 1], runs[i]);
    if (direct) {
        uint64_t total = 0;
        for (size_t i = 0; i < runs.size() && total < size; i += 2) {
            const uint64_t at = *run_offset(impl_->region, impl_->fs, runs[i + 1], runs[i]);
            const uint64_t bytes = std::min<uint64_t>(uint64_t{runs[i]} * cluster, size - total);
            for (uint64_t off = 0; off < bytes;) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, bytes - off));
                const size_t io = io_length(impl_->region, n);
                if (pread_all(impl_->region.fd, impl_->buf.get(), io, at + off, impl_->region.name, impl_->region.direct) != io)
                    throw std::runtime_error("reading " + path + ": short read on " + impl_->region.name);
                sink(impl_->buf.get(), n);
                off += n;
            }
            total += bytes;
        }
        if (total < size) throw std::runtime_error("reading " + path + ": its cluster chain is shorter than its size");
        return total;
    }
    uint64_t total = 0;
    for (;;) {
        UINT got = 0;
        check(f_read(&f, impl_->buf.get(), static_cast<UINT>(kChunk), &got), "reading " + path);
        if (got == 0) break;
        sink(impl_->buf.get(), got);
        total += got;
    }
    return total;
}

bool FatVolume::exfat() const { return impl_->fs.fs_type == FS_EXFAT; }

std::string FatVolume::label() {
    char l[34] = {};
    check(f_getlabel("", l, nullptr), "reading the volume label");
    return l;
}

uint64_t FatVolume::free_bytes() {
    DWORD clusters = 0;
    FATFS* fs = nullptr;
    check(f_getfree("", &clusters, &fs), "reading free space");
    return uint64_t{clusters} * fs->csize * impl_->region.sector;
}

void FatVolume::sync() {
    if (!impl_->region.mem) sync_fd(impl_->region.fd, impl_->region.name);
}

}  // namespace kiln

// ---- FatFs disk I/O layer: drive 0 = the bound region ---------------------

extern "C" {

DSTATUS disk_initialize(BYTE pdrv) { return (pdrv == 0 && kiln::g_region) ? 0 : STA_NOINIT; }

DSTATUS disk_status(BYTE pdrv) { return (pdrv == 0 && kiln::g_region) ? 0 : STA_NOINIT; }

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    kiln::Region* r = kiln::g_region;
    if (pdrv != 0 || !r) return RES_NOTRDY;
    if (uint64_t{sector} + count > r->sectors) return RES_PARERR;
    if (r->mem) {
        std::memcpy(buff, r->mem + uint64_t{sector} * r->sector, size_t{count} * r->sector);
        return RES_OK;
    }
    if (count == 1) {
        if (const auto c = r->cache.find(sector); c != r->cache.end()) {
            std::memcpy(buff, c->second.data(), r->sector);
            return RES_OK;
        }
    }
    try {
        BYTE* const start = buff;
        uint64_t off = r->offset + uint64_t{sector} * r->sector;
        size_t left = size_t{count} * r->sector;
        while (left > 0) {
            const size_t n = std::min(left, r->bounce.size());
            unsigned char* dst = r->aligned ? r->bounce.get() : buff;
            if (kiln::pread_all(r->fd, dst, n, off, r->name, r->direct) != n) return RES_ERROR;
            if (r->aligned) std::memcpy(buff, dst, n);
            buff += n;
            off += n;
            left -= n;
        }
        if (count == 1) kiln::remember(*r, sector, start);
        return RES_OK;
    } catch (const std::exception&) {
        return RES_ERROR;
    }
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    kiln::Region* r = kiln::g_region;
    if (pdrv != 0 || !r) return RES_NOTRDY;
    if (uint64_t{sector} + count > r->sectors) return RES_PARERR;
    if (r->mem) {
        std::memcpy(r->mem + uint64_t{sector} * r->sector, buff, size_t{count} * r->sector);
        return RES_OK;
    }
    kiln::forget(*r, sector, count);  // before writing: a failed write leaves the medium unknown
    try {
        const BYTE* const start = buff;
        uint64_t off = r->offset + uint64_t{sector} * r->sector;
        size_t left = size_t{count} * r->sector;
        while (left > 0) {
            const size_t n = std::min(left, r->bounce.size());
            const unsigned char* src = buff;
            if (r->aligned) {
                std::memcpy(r->bounce.get(), buff, n);
                src = r->bounce.get();
            }
            kiln::pwrite_all(r->fd, src, n, off, r->name);
            buff += n;
            off += n;
            left -= n;
        }
        if (count == 1) kiln::remember(*r, sector, start);
        return RES_OK;
    } catch (const std::exception&) {
        return RES_ERROR;
    }
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    kiln::Region* r = kiln::g_region;
    if (pdrv != 0 || !r) return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC:
            if (r->mem) return RES_OK;
            try {
                kiln::sync_fd(r->fd, r->name);
                return RES_OK;
            } catch (const std::exception&) {
                return RES_ERROR;
            }
        case GET_SECTOR_COUNT:
            *static_cast<LBA_t*>(buff) = static_cast<LBA_t>(r->sectors);
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD*>(buff) = static_cast<WORD>(r->sector);
            return RES_OK;
        case GET_BLOCK_SIZE:  // "erase block", in sectors: align the data area to 1 MiB
            *static_cast<DWORD*>(buff) = static_cast<DWORD>((1u << 20) / r->sector);
            return RES_OK;
    }
    return RES_PARERR;
}

DWORD get_fattime(void) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    if (tm.tm_year < 80) tm.tm_year = 80;
    return static_cast<DWORD>(tm.tm_year - 80) << 25 | static_cast<DWORD>(tm.tm_mon + 1) << 21 |
           static_cast<DWORD>(tm.tm_mday) << 16 | static_cast<DWORD>(tm.tm_hour) << 11 |
           static_cast<DWORD>(tm.tm_min) << 5 | static_cast<DWORD>(tm.tm_sec / 2);
}

}  // extern "C"
