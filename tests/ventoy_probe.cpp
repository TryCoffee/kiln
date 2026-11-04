// ventoy_probe.cpp — Ventoy disks, checked against Ventoy's own arithmetic and
// files and by independent tools: sfdisk (tables), fsck.exfat (data
// partition), fsck.fat + mtools (VTOYEFI), xz (Ventoy's images unpacked
// independently). Without those tools the probe exits 77 (ctest: skipped).
#include "check.hpp"
#include "ventoy.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace kiln;
using probe::check;

namespace {

using Bytes = std::vector<unsigned char>;
constexpr uint64_t kMiB = 1u << 20;

std::string run(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen((cmd + " 2>&1").c_str(), "r")) {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, p)) > 0;) out.append(buf, n);
        if (pclose(p) != 0) out = "EXIT-FAILED " + out;
    }
    return out;
}

// The line of `out` that contains `key`, or empty.
std::string line_with(const std::string& out, const std::string& key) {
    const size_t at = out.find(key);
    if (at == std::string::npos) return {};
    const size_t start = out.rfind('\n', at) == std::string::npos ? 0 : out.rfind('\n', at) + 1;
    return out.substr(start, out.find('\n', at) - start);
}

bool have(const char* tool) { return std::system((std::string("command -v ") + tool + " >/dev/null 2>&1").c_str()) == 0; }

Bytes read_at(const fs::path& f, uint64_t at, size_t n) {
    Bytes b(n);
    std::ifstream in(f, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

Bytes read_all(const fs::path& f) { return read_at(f, 0, static_cast<size_t>(fs::file_size(f))); }

fs::path sized_file(const fs::path& p, uint64_t bytes) {
    { std::ofstream(p).flush(); }
    fs::resize_file(p, bytes);
    return p;
}

// VentoyWorker.sh / ventoy_lib.sh (format_ventoy_disk_mbr / _gpt), transcribed
// line by line: what Kiln's layout must equal.
VentoyLayout script_layout(uint64_t sector_num, bool gpt, uint64_t reserve_mb) {
    const uint64_t VENTOY_SECTOR_NUM = 65536;
    uint64_t part1_end_sector;
    if (!gpt) {
        if (reserve_mb > 0) part1_end_sector = sector_num - reserve_mb * 2048 - VENTOY_SECTOR_NUM - 1;
        else part1_end_sector = sector_num - VENTOY_SECTOR_NUM - 1;
    } else {
        if (reserve_mb > 0) part1_end_sector = sector_num - (reserve_mb * 2048 + 33) - VENTOY_SECTOR_NUM - 1;
        else part1_end_sector = sector_num - VENTOY_SECTOR_NUM - 34;
    }
    uint64_t part2_start_sector = part1_end_sector + 1;
    const uint64_t modsector = part2_start_sector % 8;
    if (modsector > 0) {
        part1_end_sector -= modsector;
        part2_start_sector = part1_end_sector + 1;
    }
    return {sector_num, 2048, part1_end_sector, part2_start_sector, part2_start_sector + VENTOY_SECTOR_NUM - 1};
}

// Sectors [first, last] of a disk image as their own file, for the fsck tools.
// Only the first 64 MiB are copied, the rest left a hole: the disks here are
// sparse files, and a new filesystem keeps everything it has at its start.
fs::path extract(const fs::path& disk, uint64_t first, uint64_t last, const fs::path& out) {
    const uint64_t sectors = last - first + 1;
    run("dd if=" + disk.string() + " of=" + out.string() + " bs=512 skip=" + std::to_string(first) +
        " count=" + std::to_string(std::min<uint64_t>(sectors, 131072)) + " status=none");
    fs::resize_file(out, sectors * 512);
    return out;
}

}  // namespace

int main() {
    // 1. The layout, against the script, on sizes real sticks have.
    for (const uint64_t bytes : {8ull << 30, 31266439168ull, 15502147584ull + 3 * 512, 64023257088ull, 2ull << 40}) {
        for (const bool gpt : {false, true})
            for (const uint64_t reserve : {uint64_t{0}, uint64_t{100}, uint64_t{4097}}) {
                const VentoyLayout k = ventoy_layout(bytes, gpt, reserve);
                const VentoyLayout s = script_layout(bytes / 512, gpt, reserve);
                check(k.part1_first == s.part1_first && k.part1_last == s.part1_last && k.part2_first == s.part2_first &&
                          k.part2_last == s.part2_last && k.part2_first % 8 == 0,
                      "layout = Ventoy's script: " + std::to_string(bytes) + " B, " + (gpt ? "GPT" : "MBR") +
                          ", reserve " + std::to_string(reserve) + " MiB");
            }
    }
    auto refused = [](auto f) {
        try {
            f();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    check(refused([] { ventoy_layout((2ull << 40) + 512, false, 0); }), "MBR refused over 2^32 sectors");
    check(!refused([] { ventoy_layout(2ull << 40, false, 0); }) && !refused([] { ventoy_layout(4ull << 40, true, 0); }),
          "MBR at exactly 2^32 sectors and GPT at 4 TiB accepted");
    check(!refused([] { ventoy_layout(ventoy_min_bytes(), true, 0); }) &&
              refused([] { ventoy_layout(ventoy_min_bytes() - kMiB, true, 0); }),
          "ventoy_min_bytes() is the smallest drive the layout accepts");
    check(refused([] { ventoy_layout(8ull << 30, false, 8193); }) && refused([] { ventoy_layout(8ull << 30, false, 8100); }),
          "reserving more than the drive (or than leaves room) refused");

    check(ventoy_label_problem("Ventoy").empty() && ventoy_label_problem("Mój dysk 1").empty() &&
              ventoy_label_problem("ABCDEFGHIJK").empty(),
          "labels accepted: Ventoy, a Polish one with spaces, 11 characters");
    for (const char* bad : {"", "ABCDEFGHIJKL", "a/b", "a:b", "a*b", "tab\there", "emoji \xF0\x9F\x98\x80"})
        check(!ventoy_label_problem(bad).empty(),
              "label refused: \"" + std::string(bad) + "\" (" + ventoy_label_problem(bad) + ")");
    check(std::string(ventoy_version()) == KILN_VENTOY_VERSION_EXPECTED, std::string("Ventoy version ") + ventoy_version());

    for (const char* tool : {"sfdisk", "fsck.exfat", "exfatlabel", "fsck.fat", "mdir", "mtype", "xz", "dd"})
        if (!have(tool)) {
            std::printf("SKIP  %s not installed: no independent check of the disks\n", tool);
            probe::finish("ventoy_probe");
            return probe::failures ? 1 : 77;
        }

    probe::TempDir tmp;
    const fs::path files = KILN_VENTOY_FILES;
    const fs::path ref_efi = tmp.path() / "ventoy.disk.img", ref_core = tmp.path() / "core.img";
    run("xz -dc " + (files / "ventoy/ventoy.disk.img.xz").string() + " > " + ref_efi.string());
    run("xz -dc " + (files / "boot/core.img.xz").string() + " > " + ref_core.string());
    const Bytes boot = read_all(files / "boot/boot.img"), core = read_all(ref_core), efi = read_all(ref_efi);
    check(boot.size() == 512 && core.size() == 2047 * 512 && efi.size() == 32 * kMiB, "Ventoy's files unpacked by xz");

    // 2. MBR, Ventoy's defaults, 4 GiB.
    {
        const fs::path disk = sized_file(tmp.path() / "mbr.img", 4ull << 30);
        uint64_t seen = 0;
        VentoyOptions o;
        o.progress = [&](const Progress& p) { seen = std::max(seen, p.done); };
        const VentoyResult r = write_ventoy_disk(disk, o);
        check(r.ok && !r.mismatch && !r.cancelled && seen > 0, "MBR disk written and verified");
        const VentoyLayout l = ventoy_layout(4ull << 30, false, 0);
        const std::string dump = run("LC_ALL=C sfdisk --dump " + disk.string());
        check(run("LC_ALL=C sfdisk --verify " + disk.string()).find("No errors detected") != std::string::npos,
              "sfdisk --verify: no errors");
        check(dump.find("label: dos") != std::string::npos, "sfdisk: MBR");
        check(dump.find("start=        2048, size=" + std::string(12 - std::to_string(l.part1_last - 2047).size(), ' ') +
                        std::to_string(l.part1_last - 2047) + ", type=7, bootable") != std::string::npos,
              "sfdisk: partition 1 from 2048, type 7, bootable");
        check(dump.find("start=" + std::string(12 - std::to_string(l.part2_first).size(), ' ') + std::to_string(l.part2_first) +
                        ", size=       65536, type=ef") != std::string::npos,
              "sfdisk: partition 2, 65536 sectors, type ef");
        const Bytes head = read_at(disk, 0, kMiB);
        check(std::memcmp(head.data(), boot.data(), 0x180) == 0 && std::memcmp(head.data() + 0x190, boot.data() + 0x190, 440 - 0x190) == 0,
              "sector 0: Ventoy's boot code (bytes 0-439, the ID at 0x180 aside)");
        check(std::memcmp(head.data() + 0x180, "XXXXXXXXXXXXXXXX", 16) != 0, "sector 0: the disk ID placeholder is filled");
        check(std::memcmp(head.data() + 512, core.data(), core.size()) == 0, "sectors 1-2047: core.img");
        check(read_at(disk, l.part2_first * 512, 32 * kMiB) == efi, "partition 2 = Ventoy's EFI image, byte for byte");
        const fs::path p1 = extract(disk, l.part1_first, l.part1_last, tmp.path() / "mbr-p1.img");
        const std::string fx = run("LC_ALL=C fsck.exfat -n " + p1.string());
        check(fx.find("EXIT-FAILED") == std::string::npos && line_with(fx, "Cluster size").ends_with(" 32 KB"),
              "fsck.exfat: partition 1 clean, 32 KiB clusters (drive up to 32 GiB)");
        check(run("exfatlabel " + p1.string()).find("Ventoy") != std::string::npos, "exfatlabel: Ventoy");
        const fs::path p2 = extract(disk, l.part2_first, l.part2_last, tmp.path() / "mbr-p2.img");
        check(run("LC_ALL=C fsck.fat -n " + p2.string()).find("EXIT-FAILED") == std::string::npos, "fsck.fat: VTOYEFI clean");
        const std::string ids = std::string(reinterpret_cast<const char*>(head.data()) + 0x180, 16) +
                                std::string(reinterpret_cast<const char*>(head.data()) + 440, 4);
        write_ventoy_disk(disk, VentoyOptions{});
        const Bytes again = read_at(disk, 0, 512);
        check(std::memcmp(again.data() + 0x180, ids.data(), 16) != 0 && std::memcmp(again.data() + 440, ids.data() + 16, 4) != 0,
              "a second install gets a new disk ID and signature");
    }

    // 3. GPT, no Secure Boot support, own label, space reserved, 34 GiB (128 KiB clusters).
    {
        const uint64_t bytes = 34ull << 30;
        const fs::path disk = sized_file(tmp.path() / "gpt.img", bytes);
        VentoyOptions o;
        o.setup.gpt = true;
        o.setup.secure_boot = false;
        o.setup.label = "Mój dysk";
        o.setup.reserve_mib = 300;
        const VentoyResult r = write_ventoy_disk(disk, o);
        check(r.ok, "GPT disk written and verified");
        const VentoyLayout l = ventoy_layout(bytes, true, 300);
        check(run("LC_ALL=C sfdisk --verify " + disk.string()).find("No errors detected") != std::string::npos,
              "sfdisk --verify: no errors");
        const std::string dump = run("LC_ALL=C sfdisk --dump " + disk.string());
        check(dump.find("label: gpt") != std::string::npos && dump.find("name=\"Ventoy\"") != std::string::npos &&
                  dump.find("name=\"VTOYEFI\", attrs=\"GUID:63\"") != std::string::npos,
              "sfdisk: GPT, Ventoy + VTOYEFI (attribute 63)");
        check(dump.find("start=" + std::string(12 - std::to_string(l.part2_first).size(), ' ') + std::to_string(l.part2_first) +
                        ", size=       65536") != std::string::npos,
              "sfdisk: VTOYEFI where the script puts it, 300 MiB + the backup table before the end");
        const Bytes head = read_at(disk, 0, kMiB);
        check(std::memcmp(head.data(), boot.data(), 92) == 0 && head[92] == 0x22 &&
                  std::memcmp(head.data() + 93, boot.data() + 93, 0x180 - 93) == 0,
              "sector 0: boot code, core.img's LBA (byte 92) = 34");
        check(head[446 + 4] == 0xEE && head[510] == 0x55 && head[511] == 0xAA, "sector 0: protective MBR");
        Bytes core_gpt(core.begin(), core.begin() + 2014 * 512);
        core_gpt[500] = 0x23;
        check(std::memcmp(head.data() + 34 * 512, core_gpt.data(), core_gpt.size()) == 0,
              "sectors 34-2047: core.img with its next-sector pointer at 35");
        const fs::path p1 = extract(disk, l.part1_first, l.part1_last, tmp.path() / "gpt-p1.img");
        const std::string fx = run("LC_ALL=C fsck.exfat -n " + p1.string());
        check(fx.find("EXIT-FAILED") == std::string::npos && line_with(fx, "Cluster size").ends_with(" 128 KB"),
              "fsck.exfat: partition 1 clean, 128 KiB clusters (drive over 32 GiB)");
        check(run("LC_ALL=C.UTF-8 exfatlabel " + p1.string()).find("Mój dysk") != std::string::npos, "exfatlabel: Mój dysk");
        const fs::path p2 = extract(disk, l.part2_first, l.part2_last, tmp.path() / "gpt-p2.img");
        check(run("LC_ALL=C fsck.fat -n " + p2.string()).find("EXIT-FAILED") == std::string::npos, "fsck.fat: VTOYEFI clean");
        const std::string m = "MTOOLS_SKIP_CHECK=1 ", ref = ref_efi.string(), got = p2.string();
        auto sha = [&](const std::string& img, const std::string& f) {
            return run(m + "mtype -i " + img + " ::" + f + " | sha256sum").substr(0, 64);
        };
        check(sha(got, "/EFI/BOOT/BOOTX64.EFI") == sha(ref, "/EFI/BOOT/grubx64_real.efi") &&
                  sha(got, "/EFI/BOOT/BOOTIA32.EFI") == sha(ref, "/EFI/BOOT/grubia32_real.efi"),
              "no Secure Boot: Ventoy's own GRUB is BOOTX64.EFI and BOOTIA32.EFI");
        const std::string dir = run("LC_ALL=C " + m + "mdir -/ -b -i " + got + " ::/");
        bool none = true;
        for (const char* f : {"mmx64.efi", "mmia32.efi", "grubx64_real.efi", "grubia32_real.efi", "grubia32.efi",
                              "ENROLL_THIS_KEY_IN_MOKMANAGER.cer"})
            none = none && dir.find(f) == std::string::npos;
        check(none, "no Secure Boot: shim's MokManager, the *_real loaders and the key file are gone");
        check(sha(got, "/EFI/BOOT/BOOTAA64.EFI") == sha(ref, "/EFI/BOOT/BOOTAA64.EFI") &&
                  sha(got, "/grub/grub.cfg") == sha(ref, "/grub/grub.cfg"),
              "no Secure Boot: everything else as Ventoy ships it");
    }

    // 4. Refusals and cancelling.
    {
        const fs::path empty = sized_file(tmp.path() / "empty.img", 0);
        check(refused([&] { write_ventoy_disk(empty, VentoyOptions{}); }), "an image file with no size refused");
        const fs::path small = sized_file(tmp.path() / "small.img", 64 * kMiB);
        check(refused([&] { write_ventoy_disk(small, VentoyOptions{}); }), "a 64 MiB drive refused");
        VentoyOptions bad;
        bad.setup.label = "far too long a label";
        const fs::path disk = sized_file(tmp.path() / "cancel.img", 2ull << 30);
        check(refused([&] { write_ventoy_disk(disk, bad); }), "a bad label refused before the drive is touched");
        std::atomic<bool> stop{false};
        VentoyOptions o;
        o.cancel = &stop;
        o.progress = [&](const Progress& p) {
            if (p.stage == Stage::writing) stop = true;
        };
        const VentoyResult r = write_ventoy_disk(disk, o);
        const Bytes head = read_at(disk, 0, kMiB);
        check(r.cancelled && !r.ok, "cancelled while writing VTOYEFI");
        check(std::all_of(head.begin(), head.end(), [](unsigned char c) { return c == 0; }),
              "a cancelled install leaves no partition table (the tables are written last)");
    }
    return probe::finish("ventoy_probe");
}
