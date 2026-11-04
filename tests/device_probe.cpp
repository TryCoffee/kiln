// device_probe.cpp — the disk survey against a fake sysfs/mountinfo/swaps tree.
//
// Each disk in the tree is one rule: a USB stick is offered, an internal SATA
// disk is not, the root disk is not even when the root is btrfs (anonymous
// 0:N in mountinfo), a USB disk under a dm-crypt /home is not, SD yes, eMMC
// no, an empty card reader no, loop/sr/zram never appear at all.
#include "check.hpp"
#include "device.hpp"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;
using namespace kiln;
using probe::check;

namespace {

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text << "\n";
}

struct Fake {
    fs::path root;
    SystemView view;

    explicit Fake(const fs::path& r) : root(r) {
        view.sys_block = root / "sys/class/block";
        view.mountinfo = root / "proc/mountinfo";
        view.swaps = root / "proc/swaps";
        view.dev = root / "dev";
        fs::create_directories(view.sys_block);
        fs::create_directories(view.dev);
    }

    // A whole disk at devices/<bus_path>/<name>, linked from class/block.
    fs::path disk(const std::string& bus_path, const std::string& name, uint64_t sectors, bool removable,
                  const std::string& vendor = "", const std::string& model = "") {
        const fs::path dir = root / "sys/devices" / bus_path / "block" / name;
        put(dir / "size", std::to_string(sectors));
        put(dir / "removable", removable ? "1" : "0");
        put(dir / "ro", "0");
        put(dir / "queue/logical_block_size", "512");
        if (!vendor.empty()) put(dir / "device/vendor", vendor + "  ");
        if (!model.empty()) put(dir / "device/model", model + "   ");
        fs::create_directory_symlink(dir, view.sys_block / name);
        put(view.dev / name, "");
        return dir;
    }

    void partition(const fs::path& disk_dir, const std::string& name) {
        put(disk_dir / name / "partition", "1");
        fs::create_directory_symlink(disk_dir / name, view.sys_block / name);
        put(view.dev / name, "");
    }

    // A dm device on top of `slave` (a partition or disk name).
    void dm(const std::string& name, const std::string& slave, const std::string& mapper_name) {
        const fs::path dir = root / "sys/devices/virtual/block" / name;
        put(dir / "size", "1000");
        fs::create_directories(dir / "slaves");
        fs::create_directory_symlink(view.sys_block / slave, dir / "slaves" / slave);
        fs::create_directory_symlink(dir, view.sys_block / name);
        put(view.dev / name, "");
        fs::create_directories(view.dev / "mapper");
        fs::create_symlink("../" + name, view.dev / "mapper" / mapper_name);
    }
};

const DiskStatus* find(const std::vector<DiskStatus>& all, const std::string& name) {
    auto it = std::find_if(all.begin(), all.end(), [&](const DiskStatus& s) { return s.device.name == name; });
    return it == all.end() ? nullptr : &*it;
}

}  // namespace

int main() {
    probe::TempDir tmp;
    Fake f(tmp.path());

    // USB stick with one partition, automounted under a label with a space.
    const fs::path sdx = f.disk("pci0000:00/0000:00:14.0/usb1/1-1/1-1:1.0/host6/target6:0:0/6:0:0:0", "sdx",
                                30310400, true, "SanDisk", "Cruzer Blade");
    f.partition(sdx, "sdx1");
    // Internal SATA disk, nothing mounted (a second OS's disk).
    f.disk("pci0000:00/0000:00:17.0/ata2/host1/target1:0:0/1:0:0:0", "sdy", 937703088, false, "ATA", "KINGSTON");
    // Root NVMe: / (btrfs), /boot, swap.
    const fs::path nvme = f.disk("pci0000:00/0000:00:1d.0/0000:3d:00.0/nvme/nvme0", "nvme0n1", 1953525168, false);
    f.partition(nvme, "nvme0n1p1");
    f.partition(nvme, "nvme0n1p2");
    f.partition(nvme, "nvme0n1p3");
    // A USB disk carrying /home through dm-crypt.
    const fs::path sdz = f.disk("pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host7/target7:0:0/7:0:0:0", "sdz",
                                976773168, false, "WD", "Elements");
    f.partition(sdz, "sdz1");
    f.dm("dm-0", "sdz1", "home");
    // SD card and eMMC on the mmc bus.
    const fs::path sd = f.disk("platform/fe320000.mmc/mmc_host/mmc1/mmc1:aaaa", "mmcblk0", 62333952, false);
    put(sd / "device/type", "SD");
    put(sd / "device/name", "SC64G");
    const fs::path emmc = f.disk("platform/fe330000.mmc/mmc_host/mmc0/mmc0:0001", "mmcblk1", 61071360, false);
    put(emmc / "device/type", "MMC");
    // USB card reader with no card in it.
    f.disk("pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/host8/target8:0:0/8:0:0:0", "sdw", 0, true, "Generic", "Reader");
    // Removable, not USB (e.g. a FireWire or eSATA dock flagging removable media).
    f.disk("pci0000:00/0000:00:1c.0/fw0/host9/target9:0:0/9:0:0:0", "sdv", 2000000, true, "Dock", "Bay");
    // Never disks to flash.
    f.disk("virtual", "loop0", 1000, false);
    f.disk("virtual", "zram0", 1000, false);
    f.disk("pci0000:00/0000:00:17.0/ata3/host2/target2:0:0/2:0:0:0", "sr0", 2097151, true, "PLDS", "DVD");

    put(f.view.mountinfo,
        "26 1 0:30 /@ / rw,relatime shared:1 - btrfs /dev/nvme0n1p3 rw,ssd\n"
        "27 26 259:1 / /boot rw,relatime shared:2 - vfat /dev/nvme0n1p1 rw\n"
        "28 26 254:0 / /home rw,relatime shared:3 - ext4 /dev/mapper/home rw\n"
        "29 26 0:25 / /tmp rw shared:4 - tmpfs tmpfs rw\n"
        "30 26 8:49 / /run/media/u/MY\\040STICK rw,nosuid shared:5 - vfat /dev/sdx1 rw\n"
        "31 26 0:5 / /dev rw shared:6 - devtmpfs devtmpfs rw");
    put(f.view.swaps,
        "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n"
        "/dev/nvme0n1p2                          partition\t33554428\t0\t\t-2\n"
        "/swapfile                               file\t\t1048572\t\t0\t\t-3");

    const auto all = survey_disks(f.view);
    const auto offered = list_devices(f.view);

    for (const char* gone : {"loop0", "zram0", "sr0", "dm-0", "sdx1", "nvme0n1p3"})
        check(find(all, gone) == nullptr, std::string(gone) + " is not surveyed at all");

    const DiskStatus* s = find(all, "sdx");
    check(s && s->eligibility == Eligibility::eligible, "USB stick sdx is offered");
    check(s && s->device.bus == Bus::usb, "sdx is on USB");
    check(s && s->device.size_bytes == 30310400ull * 512, "sdx size from sectors");
    check(s && s->device.vendor == "SanDisk" && s->device.model == "Cruzer Blade", "vendor/model trimmed");
    check(s && s->device.mounts.size() == 1 && s->device.mounts[0].mountpoint == "/run/media/u/MY STICK",
          "sdx's mounted partition is reported, \\040 decoded");

    s = find(all, "sdy");
    check(s && s->eligibility == Eligibility::fixed_disk, "internal SATA sdy is NOT offered (fixed, unmounted)");
    check(s && s->device.bus == Bus::internal && s->device.vendor.empty(), "sdy: bus internal, libata's \"ATA\" vendor dropped");

    s = find(all, "nvme0n1");
    check(s && s->eligibility == Eligibility::system_disk, "root NVMe is a system disk despite btrfs 0:30");
    check(s && s->reason.find("holds / ") == 0, "the reason names / (" + (s ? s->reason : "") + ")");

    s = find(all, "sdz");
    check(s && s->eligibility == Eligibility::system_disk, "USB disk under dm-crypt /home is a system disk");
    check(s && s->device.mounts.size() == 1, "sdz carries the /home mount through dm-0");

    s = find(all, "mmcblk0");
    check(s && s->eligibility == Eligibility::eligible && s->device.bus == Bus::sd_card, "SD card offered");
    check(s && s->device.model == "SC64G", "SD card name from device/name");
    s = find(all, "mmcblk1");
    check(s && s->eligibility == Eligibility::fixed_disk, "eMMC is NOT offered");

    s = find(all, "sdw");
    check(s && s->eligibility == Eligibility::no_media, "empty card reader is 'no media'");

    s = find(all, "sdv");
    check(s && s->eligibility == Eligibility::eligible && s->device.bus == Bus::removable,
          "removable non-USB drive offered as removable");

    std::vector<std::string> names;
    for (const auto& d : offered) names.push_back(d.name);
    check((names == std::vector<std::string>{"mmcblk0", "sdv", "sdx"}), "list_devices = exactly the offered ones, sorted");

    check(disks_holding_path("/run/media/u/MY STICK/win.iso", f.view) == std::vector<std::string>{"sdx"},
          "an image on the stick is held by sdx");
    check(disks_holding_path("/home/u/Downloads/arch.iso", f.view) == std::vector<std::string>{"sdz"},
          "an image under /home is held by sdz (through dm-0)");
    check(disks_holding_path("/opt/x.iso", f.view) == std::vector<std::string>{"nvme0n1"},
          "an image elsewhere is held by the root disk");

    check(format_size(512) == "512 B" && format_size(15518924800ull) == "14.5 GiB", "format_size");

    return probe::finish("device_probe");
}
