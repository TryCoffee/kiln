// fat_probe.cpp — FAT32 built by FatFs through Kiln's disk glue, checked by
// FatFs itself and by two independent implementations: fsck.fat (dosfstools)
// and mtools. Without those tools the probe exits 77 (ctest: skipped). File
// data bypasses FatFs (fat.cpp): mtools must read it from the right clusters,
// and a file mtools fragmented must read back whole, run by run. FatFs's
// sector cache (fat.cpp) must stay true over 300 files in one directory.
// exFAT (Ventoy's data partition) is checked by fsck.exfat, and a FAT16 image
// edited in memory (Ventoy's VTOYEFI without Secure Boot) by fsck.fat and mtools.
#include "blockio.hpp"
#include "check.hpp"
#include "fat.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace kiln;
using probe::check;

namespace {

using Bytes = std::vector<unsigned char>;

Bytes random_bytes(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    Bytes b(n);
    for (auto& c : b) c = static_cast<unsigned char>(rng());
    return b;
}

std::string run(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen((cmd + " 2>&1").c_str(), "r")) {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, p)) > 0;) out.append(buf, n);
        if (pclose(p) != 0) out = "EXIT-FAILED " + out;
    }
    return out;
}

bool have(const char* tool) { return std::system((std::string("command -v ") + tool + " >/dev/null 2>&1").c_str()) == 0; }

}  // namespace

int main() {
    probe::TempDir tmp;
    const uint64_t mib = 1u << 20;
    const Bytes big = random_bytes(9 * mib + 123, 1);
    const std::string polish = "Zażółć gęślą jaźń.txt";
    const Bytes small = random_bytes(5000, 2);
    std::vector<Bytes> many;  // directory entries over many clusters, a FAT update per file
    for (unsigned i = 0; i < 300; ++i) many.push_back(random_bytes(3000 + i, 100 + i));
    auto many_name = [](size_t i) { return "/many/manifest-" + std::to_string(i) + ".xml"; };

    // offset 0 (a bare FAT image, for fsck.fat) and offset 1 MiB (as on a stick, for mtools)
    for (const uint64_t offset : {uint64_t{0}, mib}) {
        const std::string tag = offset ? " [at 1 MiB]" : " [at 0]";
        const fs::path img = tmp.path() / (offset ? "stick.img" : "fat.img");
        { std::ofstream(img).flush(); }
        fs::resize_file(img, 300 * mib);
        Target t = open_target(img, Access::read_write, false);
        const uint64_t length = 300 * mib - offset - mib;

        FatVolume::format(t, offset, length, "KILN_TEST");
        {
            FatVolume vol(t, offset, length);
            check(vol.label() == "KILN_TEST", "label set" + tag);
            vol.make_dir("/efi");
            vol.make_dir("/efi/boot");
            vol.make_dir("/sources");
            vol.make_dir("/sources");  // existing directory is fine
            auto from = [](const Bytes& b) {
                return [&b](void* buf, size_t n, uint64_t off) { std::memcpy(buf, b.data() + off, n); };
            };
            uint64_t reported = 0;
            vol.write_file("/sources/install.swm", big.size(), from(big), [&](size_t n) { reported += n; });
            check(reported == big.size(), "write progress adds up to the size" + tag);
            vol.write_file("/efi/boot/bootx64.efi", small.size(), from(small));
            vol.write_file("/" + polish, small.size(), from(small));
            vol.write_file("/empty.txt", 0, from(small));
            vol.make_dir("/many");
            for (size_t i = 0; i < many.size(); ++i) vol.write_file(many_name(i), many[i].size(), from(many[i]));
            bool dup = false;
            try {
                vol.write_file("/empty.txt", 0, from(small));
            } catch (const std::exception&) {
                dup = true;
            }
            check(dup, "an existing file is not overwritten" + tag);
            const uint64_t free_before = vol.free_bytes();
            bool cancelled = false;
            try {
                vol.write_file("/cancelled.bin", big.size(), from(big), [](size_t) { throw std::runtime_error("cancel"); });
            } catch (const std::runtime_error&) {
                cancelled = true;
            }
            check(cancelled && vol.free_bytes() == free_before, "a cancelled write gives its clusters back" + tag);
            vol.sync();
        }
        {
            FatVolume vol(t, offset, length);  // fresh mount
            Bytes got;
            const uint64_t n = vol.read_file("/sources/install.swm",
                                             [&](const unsigned char* d, size_t k) { got.insert(got.end(), d, d + k); });
            check(n == big.size() && got == big, "FatFs reads the 9 MiB file back identical" + tag);
            got.clear();
            vol.read_file("/" + polish, [&](const unsigned char* d, size_t k) { got.insert(got.end(), d, d + k); });
            check(got == small, "UTF-8 long name round-trips" + tag);
            bool gone = false;
            try {
                vol.read_file("/cancelled.bin", [](const unsigned char*, size_t) {});
            } catch (const std::runtime_error&) {
                gone = true;
            }
            check(gone, "a cancelled write leaves no file" + tag);
            size_t same = 0;
            for (size_t i = 0; i < many.size(); ++i) {
                got.clear();
                vol.read_file(many_name(i), [&](const unsigned char* d, size_t k) { got.insert(got.end(), d, d + k); });
                same += got == many[i];
            }
            check(same == many.size(), "300 files in one directory read back identical (" + std::to_string(same) + ")" + tag);
            check(vol.free_bytes() > 250 * mib && vol.free_bytes() < 300 * mib, "free space plausible" + tag);
        }
        bool second = false;
        try {
            FatVolume a(t, offset, length);
            FatVolume b(t, offset, length);
        } catch (const std::logic_error&) {
            second = true;
        }
        check(second, "a second FatVolume at the same time is refused" + tag);
    }

    if (!have("fsck.fat") || !have("mdir") || !have("mtype")) {
        std::printf("SKIP  fsck.fat / mtools not installed: no independent check\n");
        probe::finish("fat_probe");
        return probe::failures ? 1 : 77;
    }
    const std::string fsck = run("LC_ALL=C fsck.fat -n -v " + (tmp.path() / "fat.img").string());
    check(fsck.find("EXIT-FAILED") == std::string::npos, "fsck.fat -n: clean");
    check(fsck.find("no label in boot sector") == std::string::npos, "fsck.fat: label in the boot sector too");
    {
        // fsck.fat does not compare the FAT32 backup boot sector (sector 6): read it directly.
        std::ifstream f(tmp.path() / "fat.img", std::ios::binary);
        char backup[11] = {};
        f.seekg(6 * 512 + 71);
        f.read(backup, 11);
        check(std::string(backup, 11) == "KILN_TEST  ", "backup boot sector carries the label too");
    }
    check(fsck.find("32 bit entries") != std::string::npos, "fsck.fat: it is FAT32 (32 bit entries)");

    const std::string stick = (tmp.path() / "stick.img").string() + "@@1M";
    const std::string root = run("LC_ALL=C.UTF-8 MTOOLS_SKIP_CHECK=1 mdir -i " + stick + " ::/");
    check(root.find("EXIT-FAILED") == std::string::npos && root.find("KILN_TEST") != std::string::npos,
          "mtools reads the volume at 1 MiB, label KILN_TEST");
    check(root.find("Zażółć gęślą jaźń.txt") != std::string::npos, "mtools sees the long UTF-8 name");
    const std::string boot = run("MTOOLS_SKIP_CHECK=1 mtype -i " + stick + " ::/efi/boot/bootx64.efi | sha256sum");
    {
        const fs::path ref = tmp.path() / "small.bin";
        std::ofstream(ref, std::ios::binary).write(reinterpret_cast<const char*>(small.data()),
                                                   static_cast<std::streamsize>(small.size()));
        const std::string expect = run("sha256sum < " + ref.string());
        check(boot.substr(0, 64) == expect.substr(0, 64), "mtools reads efi/boot/bootx64.efi byte-identical");
    }
    auto save = [&](const std::string& name, const Bytes& b) {
        const fs::path p = tmp.path() / name;
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        return p.string();
    };
    const std::string big_ref = save("big.bin", big);
    {
        const std::string list = run("LC_ALL=C.UTF-8 MTOOLS_SKIP_CHECK=1 mdir -b -i " + stick + " ::/many");
        check(std::count(list.begin(), list.end(), '\n') == 300, "mtools lists the 300 files in /many");
        const std::string last = save("many-299.bin", many[299]);
        check(run("MTOOLS_SKIP_CHECK=1 mtype -i " + stick + " ::" + many_name(299) + " | sha256sum").substr(0, 64) ==
                  run("sha256sum < " + last).substr(0, 64),
              "mtools reads the last of them byte-identical");
    }
    check(run("MTOOLS_SKIP_CHECK=1 mtype -i " + stick + " ::/sources/install.swm | sha256sum").substr(0, 64) ==
              run("sha256sum < " + big_ref).substr(0, 64),
          "mtools reads the 9 MiB file byte-identical (written straight to its clusters)");

    {  // a hole between two files, then a file larger than it with only a little room elsewhere: mtools fragments it
        const std::string m = "MTOOLS_SKIP_CHECK=1 ";
        const std::string hole = save("hole.bin", random_bytes(mib, 3));
        const Bytes frag = random_bytes(3 * mib, 4);
        const std::string frag_ref = save("frag.bin", frag);
        run(m + "mcopy -i " + stick + " " + hole + " ::/before.bin");
        run(m + "mcopy -i " + stick + " " + hole + " ::/hole.bin");
        run(m + "mcopy -i " + stick + " " + hole + " ::/after.bin");
        const uint64_t length = 300 * mib - 2 * mib;
        uint64_t free_now = 0;
        {
            Target t = open_target(tmp.path() / "stick.img", Access::read_write, false);
            free_now = FatVolume(t, mib, length).free_bytes();
        }
        const fs::path filler = tmp.path() / "filler.bin";
        { std::ofstream(filler).flush(); }
        fs::resize_file(filler, free_now - 2 * mib);
        run(m + "mcopy -i " + stick + " " + filler.string() + " ::/filler.bin");
        run(m + "mdel -i " + stick + " ::/hole.bin");
        run(m + "mcopy -i " + stick + " " + frag_ref + " ::/frag.bin");
        const std::string chain = run(m + "mshowfat -i " + stick + " ::/frag.bin");
        check(std::count(chain.begin(), chain.end(), '<') >= 2, "mtools wrote frag.bin in pieces: " + chain.substr(0, chain.find('\n')));
        Target t = open_target(tmp.path() / "stick.img", Access::read, false);
        FatVolume vol(t, mib, length);
        Bytes got;
        const uint64_t n = vol.read_file("/frag.bin", [&](const unsigned char* d, size_t k) { got.insert(got.end(), d, d + k); });
        check(n == frag.size() && got == frag, "a fragmented file reads back identical, run by run");
    }

    if (!have("fsck.exfat") || !have("exfatlabel") || !have("mkfs.fat")) {
        std::printf("SKIP  fsck.exfat / exfatlabel / mkfs.fat not installed: exFAT and memory images not checked\n");
        probe::finish("fat_probe");
        return probe::failures ? 1 : 77;
    }
    {  // exFAT as Ventoy's data partition gets it: label as typed, the cluster size asked for
        const fs::path img = tmp.path() / "exfat.img";
        { std::ofstream(img).flush(); }
        fs::resize_file(img, 300 * mib);
        Target t = open_target(img, Access::read_write, false);
        FatVolume::format(t, 0, 300 * mib, "Pendrive 1", FatType::exfat, 32768);
        // Checked before any file is written: FatFs zeroes a written file's
        // last-access time on exFAT, which exfat-utils' fsck reports.
        const std::string fsck_ex = run("LC_ALL=C fsck.exfat -n " + img.string());
        check(fsck_ex.find("EXIT-FAILED") == std::string::npos, "fsck.exfat: the new exFAT volume is clean");
        check(probe::exfat_cluster_bytes(img) == 32768, "exFAT boot sector: 32 KiB clusters");
        check(run("exfatlabel " + img.string()).find("Pendrive 1") != std::string::npos, "exfatlabel reads the label");
        {
            FatVolume vol(t, 0, 300 * mib);
            check(vol.exfat() && vol.label() == "Pendrive 1", "FatFs mounts it as exFAT, label kept as typed");
            vol.write_file("/big.bin", big.size(),
                           [&](void* buf, size_t n, uint64_t off) { std::memcpy(buf, big.data() + off, n); });
        }
        FatVolume vol(t, 0, 300 * mib);
        Bytes got;
        vol.read_file("/big.bin", [&](const unsigned char* d, size_t k) { got.insert(got.end(), d, d + k); });
        check(got == big, "a file written to exFAT reads back identical");
    }
    {  // a FAT16 image held in memory: files removed and added there, then read by mtools
        const fs::path img = tmp.path() / "efi.img";
        const std::string m = "MTOOLS_SKIP_CHECK=1 ";
        run("mkfs.fat -F 16 -n VTOYEFI -C " + img.string() + " 32768");
        const std::string a = save("a.efi", random_bytes(70000, 5));
        run(m + "mmd -i " + img.string() + " ::/EFI");
        run(m + "mcopy -i " + img.string() + " " + a + " ::/EFI/real.efi");
        run(m + "mcopy -i " + img.string() + " " + a + " ::/EFI/old.efi");
        Bytes image(fs::file_size(img));
        std::ifstream(img, std::ios::binary).read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image.size()));
        {
            FatVolume v(image.data(), image.size());
            check(v.exists("/EFI/real.efi") && !v.exists("/EFI/none.efi"), "memory image: exists() sees what is there");
            Bytes real;
            v.read_file("/EFI/real.efi", [&](const unsigned char* d, size_t k) { real.insert(real.end(), d, d + k); });
            check(v.remove("/EFI/old.efi") && !v.remove("/EFI/old.efi"), "memory image: remove() once, then nothing to remove");
            v.write_file("/EFI/BOOTX64.EFI", real.size(),
                         [&](void* buf, size_t n, uint64_t off) { std::memcpy(buf, real.data() + off, n); });
        }
        std::ofstream(img, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        check(run("LC_ALL=C fsck.fat -n " + img.string()).find("EXIT-FAILED") == std::string::npos, "memory image: fsck.fat clean");
        const std::string dir = run("LC_ALL=C " + m + "mdir -b -i " + img.string() + " ::/EFI");
        check(dir.find("old.efi") == std::string::npos && dir.find("BOOTX64.EFI") != std::string::npos,
              "memory image: mtools sees old.efi gone and BOOTX64.EFI added");
        check(run(m + "mtype -i " + img.string() + " ::/EFI/BOOTX64.EFI | sha256sum").substr(0, 64) ==
                  run("sha256sum < " + a).substr(0, 64),
              "memory image: mtools reads BOOTX64.EFI byte-identical");
    }
    return probe::finish("fat_probe");
}
