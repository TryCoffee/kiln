// win_device_probe.cpp — the Windows raw-disk path on a VHD.
//
// Needs an administrator (diskpart, raw disk access); otherwise exit 77
// (skipped). Attaches a 64 MiB VHD with a mounted FAT32 volume, then: the
// survey must list it but not offer it (virtual disk) and report its drive
// letter; the disk holding C:\Windows must be a system disk; unmount_device
// must lock and dismount the volume; a writing open must lock it again while
// it is mounted; a raw write of an image whose length is not a sector multiple
// must verify and read back through a second, cached handle; a corrupted byte
// must be found; FAT32 must build on the raw disk.
#include "blockio.hpp"
#include "check.hpp"
#include "device.hpp"
#include "fat.hpp"
#include "writer.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <vector>

namespace fs = std::filesystem;
using namespace kiln;
using probe::check;

namespace {

std::string diskpart(const fs::path& dir, const std::string& script) {
    const fs::path s = dir / "diskpart.txt";
    std::ofstream(s) << script;
    std::string out;
    if (FILE* p = _popen(("diskpart /s \"" + s.string() + "\" 2>&1").c_str(), "r")) {
        char buf[512];
        while (fgets(buf, sizeof buf, p)) out += buf;
        _pclose(p);
    }
    return out;
}

std::set<std::string> disk_names() {
    std::set<std::string> n;
    for (const auto& st : survey_disks()) n.insert(st.device.name);
    return n;
}

std::optional<DiskStatus> find(const std::string& name) {
    for (auto& st : survey_disks())
        if (st.device.name == name) return st;
    return std::nullopt;
}

}  // namespace

namespace {

int run() {
    if (!running_as_admin()) {
        std::printf("SKIP  needs an administrator (diskpart, raw disk access): run it from an elevated terminal\n");
        return 77;
    }

    bool system_found = false;
    std::string system_name;
    auto lower = [](std::string x) {
        for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return x;
    };
    for (const auto& st : survey_disks())  // "holds C:\WINDOWS": Windows reports the folder in capitals
        if (st.eligibility == Eligibility::system_disk && lower(st.reason).find("\\windows") != std::string::npos) {
            system_found = true;
            system_name = st.device.name;
        }
    check(system_found, "the disk holding the Windows folder is a system disk (" + system_name + ")");
    const auto holders = disks_holding_path("C:\\Windows\\System32");
    check(std::find(holders.begin(), holders.end(), system_name) != holders.end(),
          "disks_holding_path(C:\\Windows\\System32) names it");

    probe::TempDir tmp;
    const fs::path vhd = tmp.path() / "probe.vhd";
    const std::set<std::string> before = disk_names();
    const std::string out = diskpart(tmp.path(), "create vdisk file=\"" + vhd.string() + "\" maximum=64 type=fixed\n"
                                                 "select vdisk file=\"" + vhd.string() + "\"\n"
                                                 "attach vdisk\n"
                                                 "create partition primary\n"
                                                 "format fs=fat32 quick label=KILNPROBE\n"
                                                 "assign\n");
    struct Detach {
        fs::path dir, vhd;
        ~Detach() { diskpart(dir, "select vdisk file=\"" + vhd.string() + "\"\ndetach vdisk\n"); }
    } detach{tmp.path(), vhd};
    std::string name;
    for (int i = 0; i < 50 && name.empty(); ++i) {  // the new disk appears within a moment
        for (const auto& n : disk_names())
            if (!before.count(n)) name = n;
        if (name.empty()) Sleep(100);
    }
    check(!name.empty(), "VHD attached as a disk: " + name);
    if (name.empty()) {
        std::printf("%s", out.c_str());
        return probe::finish("win_device_probe");
    }
    std::optional<DiskStatus> st;
    for (int i = 0; i < 50; ++i) {  // and its volume a moment later
        st = find(name);
        if (st && !st->device.mounts.empty()) break;
        Sleep(100);
    }
    check(st && st->eligibility != Eligibility::eligible && st->reason == "virtual disk (VHD)",
          "the survey lists the VHD and does not offer it (" + (st ? st->reason : std::string("missing")) + ")");
    check(st && st->device.size_bytes == (64u << 20), "its size is 64 MiB");
    check(st && !st->device.mounts.empty(), "its FAT32 volume is reported" +
                                                (st && !st->device.mounts.empty() ? ": " + st->device.mounts[0].mountpoint : ""));
    if (!st) return probe::finish("win_device_probe");
    const std::string path = st->device.path;

    std::string why;
    std::vector<std::string> reported;
    const bool unmounted = unmount_device(st->device, &why, [&](const std::string& mp) { reported.push_back(mp); });
    check(unmounted && reported.size() == st->device.mounts.size(), "unmount_device locks and dismounts it" + (why.empty() ? "" : ": " + why));

    {  // using the volume mounts it again: the writing open must cope with a live file system
        const fs::path f = fs::path(st->device.mounts[0].mountpoint) / "remount.txt";
        { std::ofstream(f) << "mounted again"; }
        std::error_code ec;
        check(fs::file_size(f, ec) == 13, "the volume is in use again (a file written to " + st->device.mounts[0].mountpoint + ")");
    }
    {
        Target t = open_target(path, Access::read_write);
        check(t.block && t.direct && t.size == (64u << 20) && t.sector == 512 && t.align == 512 && t.held,
              "a writing open: raw disk, unbuffered, 512-byte sectors, volume locks held");
    }

    std::vector<unsigned char> image(9 * (1u << 20) + 123);
    std::mt19937 rng(7);
    for (auto& c : image) c = static_cast<unsigned char>(rng());
    const fs::path img = tmp.path() / "image.bin";
    std::ofstream(img, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    const WriteResult r = write_image(img, path, {});
    check(r.ok && r.direct, "raw write (length not a sector multiple) + read-back verify through " + path);

    {  // a second, cached handle reads the same bytes
        Fd h(CreateFileW(fs::path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        std::vector<unsigned char> back(round_up(image.size(), 512));
        const size_t got = h.valid() ? pread_all(h.get(), back.data(), back.size(), 0, path, false) : 0;
        check(got == back.size() && std::equal(image.begin(), image.end(), back.begin()), "a cached handle reads the image back");
    }
    {  // corrupt one byte on the disk: verify must name it
        Target t = open_target(path, Access::read_write);
        AlignedBuffer s(512);
        pread_all(t.fd.get(), s.get(), 512, 4096 * 100, t.name, true);
        s.get()[17] ^= 0xFF;
        pwrite_all(t.fd.get(), s.get(), 512, 4096 * 100, t.name);
    }
    const WriteResult v = verify_image(img, path);
    check(!v.ok && v.mismatch_at && *v.mismatch_at == 4096 * 100 + 17, "a corrupted byte on the disk is found at its offset");

    {  // FAT32 on the raw disk: FatFs's unaligned buffers go through the bounce buffer
        Target t = open_target(path, Access::read_write);
        FatVolume::format(t, 1u << 20, 62u << 20, "KILN_WIN");
        const std::vector<unsigned char> small(100000, 0x5A);
        {
            FatVolume vol(t, 1u << 20, 62u << 20);
            vol.make_dir("/efi");
            vol.write_file("/efi/test.bin", small.size(), [&](void* b, size_t n, uint64_t off) { std::memcpy(b, small.data() + off, n); });
            vol.sync();
        }
        FatVolume vol(t, 1u << 20, 62u << 20);
        std::vector<unsigned char> got;
        vol.read_file("/efi/test.bin", [&](const unsigned char* d, size_t n) { got.insert(got.end(), d, d + n); });
        check(got == small && vol.label() == "KILN_WIN", "FAT32 built and read back on the raw disk");
    }
    check(reread_partition_table(path), "IOCTL_DISK_UPDATE_PROPERTIES accepted");
    return probe::finish("win_device_probe");
}

}  // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        check(false, std::string("threw: ") + e.what());
        return probe::finish("win_device_probe");
    }
}
