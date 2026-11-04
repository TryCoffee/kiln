// ventoy_write.cpp — writes a Ventoy disk (layout in ventoy.hpp). Ventoy's
// boot.img, core.img.xz and ventoy.disk.img.xz are linked in by CMake
// (ventoy_blobs.c, generated with #embed) and unpacked with XZ Embedded.
#include "ventoy.hpp"

#include "fat.hpp"
#include "gpt.hpp"

#include <xz.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

extern "C" {
extern const unsigned char kiln_ventoy_boot_img[];
extern const size_t kiln_ventoy_boot_img_size;
extern const unsigned char kiln_ventoy_core_img_xz[];
extern const size_t kiln_ventoy_core_img_xz_size;
extern const unsigned char kiln_ventoy_disk_img_xz[];
extern const size_t kiln_ventoy_disk_img_xz_size;
}

namespace kiln {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint64_t kMiB = 1u << 20;
constexpr size_t kCoreBytes = 2047 * 512;   // core.img: sectors 1-2047
constexpr size_t kEfiBytes = 32 * kMiB;     // ventoy.disk.img: partition 2
constexpr size_t kChunk = 4 * kMiB;
constexpr uint64_t kEfiAttributes = 0x8000000000000000ull;  // as Ventoy: basic data "no drive letter"

struct Cancelled {};

double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::vector<unsigned char> unxz(const unsigned char* data, size_t n, size_t expect, const char* what) {
    xz_crc32_init();
    std::unique_ptr<xz_dec, void (*)(xz_dec*)> dec(xz_dec_init(XZ_SINGLE, 0), xz_dec_end);
    if (!dec) throw std::bad_alloc();
    std::vector<unsigned char> out(expect);
    xz_buf b{data, 0, n, out.data(), 0, out.size()};
    const xz_ret r = xz_dec_run(dec.get(), &b);
    if (r != XZ_STREAM_END || b.in_pos != n || b.out_pos != expect)
        throw std::runtime_error(std::string("Kiln's copy of Ventoy's ") + what + " is damaged (xz error " +
                                 std::to_string(static_cast<int>(r)) + ")");
    return out;
}

// Ventoy without Secure Boot support, as its installer does it
// (VentoyProcSecureBoot): its own GRUB becomes the boot loader, and shim,
// MokManager and the key to enrol go.
void drop_secure_boot(std::vector<unsigned char>& image) {
    struct Arch {
        const char* real;
        const char* boot;
        std::vector<const char*> drop;
    };
    const Arch arches[] = {
        {"/EFI/BOOT/grubx64_real.efi", "/EFI/BOOT/BOOTX64.EFI",
         {"/EFI/BOOT/grubx64.efi", "/EFI/BOOT/MokManager.efi", "/EFI/BOOT/mmx64.efi",
          "/ENROLL_THIS_KEY_IN_MOKMANAGER.cer", "/EFI/BOOT/grub.efi"}},
        {"/EFI/BOOT/grubia32_real.efi", "/EFI/BOOT/BOOTIA32.EFI", {"/EFI/BOOT/grubia32.efi", "/EFI/BOOT/mmia32.efi"}},
    };
    FatVolume v(image.data(), image.size());
    for (const Arch& a : arches) {
        if (!v.exists(a.real)) throw std::runtime_error(std::string("Ventoy's EFI partition has no ") + a.real);
        std::vector<unsigned char> loader;
        v.read_file(a.real, [&](const unsigned char* d, size_t n) { loader.insert(loader.end(), d, d + n); });
        v.remove(a.boot);
        for (const char* d : a.drop) v.remove(d);
        v.remove(a.real);
        v.write_file(a.boot, loader.size(),
                     [&](void* buf, size_t n, uint64_t off) { std::memcpy(buf, loader.data() + off, n); });
    }
}

void put32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}

// CHS of an LBA for an MBR entry (255 heads, 63 sectors), 1023/254/63 beyond.
void put_chs(unsigned char* p, uint64_t lba) {
    uint64_t c = lba / (255 * 63), h = lba / 63 % 255, s = lba % 63 + 1;
    if (c > 1023) c = 1023, h = 254, s = 63;
    p[0] = static_cast<unsigned char>(h);
    p[1] = static_cast<unsigned char>(s | ((c >> 2) & 0xC0));
    p[2] = static_cast<unsigned char>(c);
}

void mbr_entry(unsigned char* e, bool active, unsigned char type, uint64_t first, uint64_t last) {
    e[0] = active ? 0x80 : 0x00;
    put_chs(e + 1, first);
    e[4] = type;
    put_chs(e + 5, last);
    put32(e + 8, static_cast<uint32_t>(first));
    put32(e + 12, static_cast<uint32_t>(last - first + 1));
}

}  // namespace

VentoyResult write_ventoy_disk(const TargetSpec& target, const VentoyOptions& o) {
    VentoyResult r;
    const VentoySetup& s = o.setup;
    auto report = [&](Stage st, uint64_t done, uint64_t total) {
        if (o.progress) o.progress({st, done, total});
    };
    auto check_cancel = [&] {
        if (o.cancel && o.cancel->load(std::memory_order_relaxed)) throw Cancelled{};
    };
    if (const std::string why = ventoy_label_problem(s.label); !why.empty()) throw std::runtime_error("Ventoy: " + why);

    // Ventoy's files, unpacked (and their checksums checked) before the drive is touched.
    if (kiln_ventoy_boot_img_size != 512) throw std::runtime_error("Kiln's copy of Ventoy's boot.img is damaged");
    std::vector<unsigned char> core = unxz(kiln_ventoy_core_img_xz, kiln_ventoy_core_img_xz_size, kCoreBytes, "core.img");
    std::vector<unsigned char> efi = unxz(kiln_ventoy_disk_img_xz, kiln_ventoy_disk_img_xz_size, kEfiBytes, "ventoy.disk.img");
    if (!s.secure_boot) drop_secure_boot(efi);

    const auto start = Clock::now();
    bool reread = false;
    try {
        // 1. The target, held open (O_EXCL on a drive) from here on.
        Target t = open_target(target, Access::read_write, /*direct_on_files=*/false, /*create=*/false);
        r.direct = t.direct;
        if (!t.block && t.size == 0)
            throw std::runtime_error("an image-file target must already have the drive's size "
                                     "(e.g. truncate -s 8G " + t.name + ")");
        if (t.sector != 512)
            throw std::runtime_error("Ventoy needs a drive with 512-byte sectors; " + t.name + " has " +
                                     std::to_string(t.sector) + "-byte sectors");
        const VentoyLayout l = ventoy_layout(t.size, s.gpt, s.reserve_mib);

        // 2. The first MiB as it will be: boot sector (+ GPT), then core.img.
        std::random_device rd;
        auto random_bytes = [&](unsigned char* p, size_t n) {
            for (size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(rd());
        };
        AlignedBuffer head(kMiB);
        std::memset(head.get(), 0, kMiB);
        std::optional<GptLayout> gpt;
        if (s.gpt) {
            gpt = make_gpt(t.size, 512,
                           {{"Ventoy", l.part1_first, l.part1_last, 0}, {"VTOYEFI", l.part2_first, l.part2_last, kEfiAttributes}});
            std::memcpy(head.get(), gpt->head.data(), gpt->head.size());  // protective MBR, header, entries
        } else {
            mbr_entry(head.get() + 446, true, 0x07, l.part1_first, l.part1_last);  // exFAT/NTFS, active
            mbr_entry(head.get() + 462, false, 0xEF, l.part2_first, l.part2_last);  // EFI system
            head.get()[510] = 0x55;
            head.get()[511] = 0xAA;
        }
        std::memcpy(head.get(), kiln_ventoy_boot_img, 440);  // the boot code, not boot.img's table
        random_bytes(head.get() + 0x180, 16);  // Ventoy's disk ID
        random_bytes(head.get() + 440, 4);     // disk signature
        if (s.gpt) {
            // GRUB's boot.img finds core.img at the LBA in byte 92, and core.img
            // the rest of itself from byte 500: both move from LBA 1 to 34.
            head.get()[92] = 0x22;
            std::memcpy(head.get() + 34 * 512, core.data(), kMiB - 34 * 512);
            head.get()[34 * 512 + 500] = 0x23;
        } else {
            std::memcpy(head.get() + 512, core.data(), kCoreBytes);
        }

        // 3. Wipe old tables at both ends, format partition 1, write partition 2,
        //    and the tables last, as Ventoy does: until then the drive carries no
        //    partition table at all.
        report(Stage::partitioning, 0, 1);
        AlignedBuffer buf(kChunk);
        std::memset(buf.get(), 0, kMiB);
        pwrite_all(t.fd.get(), buf.get(), kMiB, 0, t.name);
        pwrite_all(t.fd.get(), buf.get(), kMiB, round_down(t.size - kMiB, t.sector), t.name);
        // Ventoy's cluster sizes: 32 KiB up to 32 GiB, 128 KiB above.
        const uint32_t cluster = t.size / (1ull << 30) > 32 ? 131072 : 32768;
        FatVolume::format(t, l.part1_first * 512, (l.part1_last - l.part1_first + 1) * 512, s.label, FatType::exfat,
                          cluster);
        report(Stage::partitioning, 1, 1);
        check_cancel();

        const uint64_t total = efi.size() + kMiB;
        uint64_t done = 0;
        for (size_t off = 0; off < efi.size(); off += kChunk) {
            std::memcpy(buf.get(), efi.data() + off, kChunk);
            pwrite_all(t.fd.get(), buf.get(), kChunk, l.part2_first * 512 + off, t.name);
            done += kChunk;
            report(Stage::writing, done, total);
            check_cancel();
        }
        if (gpt) {
            AlignedBuffer tail(gpt->tail.size());
            std::memcpy(tail.get(), gpt->tail.data(), gpt->tail.size());
            pwrite_all(t.fd.get(), tail.get(), gpt->tail.size(), gpt->tail_lba * 512, t.name);
        }
        pwrite_all(t.fd.get(), head.get(), kMiB, 0, t.name);
        report(Stage::writing, total, total);
        report(Stage::syncing, 0, 1);
        sync_fd(t.fd.get(), t.name);
        report(Stage::syncing, 1, 1);
        r.write_seconds = seconds_since(start);

        // 4. Verify: every byte Kiln wrote outside partition 1 read back, and the
        //    exFAT volume mounted afresh.
        if (o.verify) {
            const auto vstart = Clock::now();
            drop_cached(t);
            uint64_t checked = 0;
            const uint64_t vtotal = total + (gpt ? gpt->tail.size() : 0);
            auto compare = [&](const char* area, uint64_t at, const unsigned char* expect, size_t n) {
                for (size_t off = 0; off < n && !r.mismatch; off += kChunk) {
                    const size_t len = std::min(kChunk, n - off);
                    if (pread_all(t.fd.get(), buf.get(), len, at + off, t.name, t.direct) != len)
                        throw std::runtime_error("reading back " + t.name + ": short read");
                    const auto diff = std::mismatch(buf.get(), buf.get() + len, expect + off);
                    if (diff.first != buf.get() + len)
                        r.mismatch = std::string(area) + " at byte " + std::to_string(off + (diff.first - buf.get()));
                    checked += len;
                    report(Stage::verifying, checked, vtotal);
                    check_cancel();
                }
            };
            compare("the boot area (first MiB)", 0, head.get(), kMiB);
            compare("the VTOYEFI partition", l.part2_first * 512, efi.data(), efi.size());
            if (gpt) compare("the backup partition table", gpt->tail_lba * 512, gpt->tail.data(), gpt->tail.size());
            if (!r.mismatch) {
                try {
                    FatVolume v(t, l.part1_first * 512, (l.part1_last - l.part1_first + 1) * 512);
                    if (!v.exfat()) r.mismatch = "the data partition (not exFAT)";
                    else if (v.label() != s.label) r.mismatch = "the data partition (label \"" + v.label() + "\")";
                } catch (const std::runtime_error& e) {
                    r.mismatch = std::string("the data partition (") + e.what() + ")";
                }
            }
            r.verify_seconds = seconds_since(vstart);
        }
        reread = t.block;
    } catch (const Cancelled&) {
        r.cancelled = true;
    }
    if (reread) r.replug = !reread_partition_table(target);  // after our O_EXCL descriptor is closed
    r.ok = !r.cancelled && !r.mismatch;
    return r;
}

}  // namespace kiln
