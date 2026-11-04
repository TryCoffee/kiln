// mac_device_probe.cpp — macOS raw-device path on a file-backed disk.
//
// Attaches a 64 MiB file as /dev/diskN (no mount), then, as root (the node is
// read-only even for its owner): the survey must not offer it; authopen must
// hand over a descriptor; a raw write of an image whose length is not a sector
// multiple must verify; a corrupted byte must be found; FAT32 must build on the
// raw node through the aligned bounce buffer. Not root: exit 77 (skipped).
#include "blockio.hpp"
#include "check.hpp"
#include "device.hpp"
#include "fat.hpp"
#include "writer.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <vector>

namespace fs = std::filesystem;
using namespace kiln;
using probe::check;

namespace {

std::string run(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r")) {
        char buf[512];
        while (fgets(buf, sizeof buf, p)) out += buf;
        pclose(p);
    }
    return out;
}

}  // namespace

int main() {
    if (::geteuid() != 0) {
        std::printf("SKIP  needs root (a file-backed disk node is read-only for its owner): sudo %s\n", "mac_device_probe");
        return 77;
    }
    probe::TempDir tmp;
    const fs::path backing = tmp.path() / "disk.img";
    { std::ofstream(backing).flush(); }
    fs::resize_file(backing, 64u << 20);
    std::string dev = run("hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage '" + backing.string() +
                          "' 2>/dev/null | awk '{print $1}' | head -1");
    while (!dev.empty() && (dev.back() == '\n' || dev.back() == ' ')) dev.pop_back();
    check(dev.rfind("/dev/disk", 0) == 0, "file attached as a disk: " + dev);
    if (dev.rfind("/dev/disk", 0) != 0) return probe::finish("mac_device_probe");
    const std::string name = dev.substr(5);
    const std::string raw = "/dev/r" + name;
    struct Detach {
        std::string dev;
        ~Detach() { run("hdiutil detach -force " + dev); }
    } detach{dev};

    bool listed = false, offered = false;
    for (const auto& st : survey_disks())
        if (st.device.name == name) {
            listed = true;
            offered = st.eligibility == Eligibility::eligible;
        }
    check(listed && !offered, "the survey sees the disk image and does not offer it");

    // Mounted: the survey must report it, unmount_device (DiskArbitration) must take it down.
    run("newfs_msdos -F 32 -v KILNPROBE " + raw);
    run("diskutil mount " + dev);
    std::optional<Device> mounted;
    for (const auto& st : survey_disks())
        if (st.device.name == name) mounted = st.device;
    check(mounted && !mounted->mounts.empty(), "the survey reports the disk's mount" +
                                                   (mounted && !mounted->mounts.empty() ? ": " + mounted->mounts[0].mountpoint : ""));
    std::string why;
    std::vector<std::string> reported;
    const bool unmounted = mounted && unmount_device(*mounted, &why, [&](const std::string& mp) { reported.push_back(mp); });
    bool still = false;
    for (const auto& st : survey_disks())
        if (st.device.name == name) still = !st.device.mounts.empty();
    check(unmounted && !still && reported.size() == 1, "unmount_device takes it down" + (why.empty() ? "" : ": " + why));

    const int fd = authopen_device(raw, &why);
    check(fd >= 0, "authopen hands over a descriptor for " + raw + (fd < 0 ? ": " + why : ""));
    if (fd < 0) return probe::finish("mac_device_probe");
    Fd held(fd);
    const TargetSpec spec = TargetSpec::descriptor(fd, raw);
    {
        Target t = open_target(spec, Access::read_write);
        check(t.block && t.size == (64u << 20) && t.sector == 512 && t.align == 512,
              "descriptor seen as a 64 MiB raw disk, 512-byte sectors, aligned I/O");
    }

    std::vector<unsigned char> image(9 * (1u << 20) + 123);
    std::mt19937 rng(7);
    for (auto& c : image) c = static_cast<unsigned char>(rng());
    const fs::path img = tmp.path() / "image.bin";
    std::ofstream(img, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    const WriteResult r = write_image(img, spec, {});
    check(r.ok && r.direct && !r.replug, "raw write (length not a sector multiple) + read-back verify through /dev/rdisk");

    {  // the medium really holds it: read the node back independently
        std::ifstream in(raw, std::ios::binary);
        std::vector<unsigned char> back(image.size());
        in.read(reinterpret_cast<char*>(back.data()), static_cast<std::streamsize>(back.size()));
        check(back == image, "reading " + raw + " back with plain I/O gives the image");
    }
    {  // corrupt one byte on the disk: verify must name it
        Target t = open_target(spec, Access::read_write);
        AlignedBuffer s(512);
        pread_all(t.fd.get(), s.get(), 512, 4096 * 100, t.name, true);
        s.get()[17] ^= 0xFF;
        pwrite_all(t.fd.get(), s.get(), 512, 4096 * 100, t.name);
    }
    const WriteResult v = verify_image(img, spec);
    check(!v.ok && v.mismatch_at && *v.mismatch_at == 4096 * 100 + 17, "a corrupted byte on the disk is found at its offset");

    {  // FAT32 on the raw node: FatFs's unaligned buffers must go through the bounce buffer
        Target t = open_target(spec, Access::read_write);
        FatVolume::format(t, 1u << 20, 62u << 20, "KILN_MAC");
        const std::vector<unsigned char> small(100000, 0x5A);
        {
            FatVolume vol(t, 1u << 20, 62u << 20);
            vol.make_dir("/efi");
            vol.write_file("/efi/test.bin", small.size(),
                           [&](void* b, size_t n, uint64_t off) { std::memcpy(b, small.data() + off, n); });
            vol.sync();
        }
        FatVolume vol(t, 1u << 20, 62u << 20);
        std::vector<unsigned char> got;
        vol.read_file("/efi/test.bin", [&](const unsigned char* d, size_t n) { got.insert(got.end(), d, d + n); });
        check(got == small && vol.label() == "KILN_MAC", "FAT32 built and read back on the raw node");
    }
    return probe::finish("mac_device_probe");
}
