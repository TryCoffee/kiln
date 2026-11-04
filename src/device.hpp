// device.hpp — the drives Kiln may write to, and why every other disk is left out.
//
// The one rule that matters most in a flasher: never offer a disk the running
// system lives on. Eligible = USB-attached, an SD card, or flagged removable by
// the kernel; AND not backing any system mount (/, /boot, /home, ...) or swap,
// followed through dm-crypt/LVM/md down to the physical disks. Fixed internal
// disks are never offered, even when nothing on them is mounted (a second
// internal SSD with another OS on it is exactly that case). Windows: the disks behind the Windows
// folder, the boot partition and every page file, and virtual disks (VHDs).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kiln {

enum class Bus { usb, sd_card, removable, internal };

struct Mount {
    std::string source;      // "/dev/sdc1"; Windows: the volume, "\\?\Volume{...}\"
    std::string mountpoint;  // "/run/media/alex/STICK"; Windows: "E:\"
};

struct Device {
    std::string name;         // "sdc"; macOS "disk4"; Windows "PhysicalDrive2"
    std::string path;         // "/dev/sdc"; macOS "/dev/rdisk4"; Windows "\\.\PhysicalDrive2"
    std::string vendor;
    std::string model;
    uint64_t size_bytes = 0;
    uint32_t logical_block = 512;
    Bus bus = Bus::usb;
    bool read_only = false;
    std::vector<Mount> mounts;  // the disk's own and its partitions', through dm/md too
};

// Why a whole disk is or is not offered (`kiln list --all`).
enum class Eligibility { eligible, system_disk, fixed_disk, no_media };

struct DiskStatus {
    Device device;
    Eligibility eligibility = Eligibility::fixed_disk;
    std::string reason;  // human-readable, e.g. "holds / (nvme0n1p3)"
};

// Where the survey reads the system's state (Linux). The defaults are the live
// system; the tests point them at a fake tree. macOS and Windows ignore it.
struct SystemView {
    std::filesystem::path sys_block = "/sys/class/block";
    std::filesystem::path mountinfo = "/proc/self/mountinfo";
    std::filesystem::path swaps = "/proc/swaps";
    std::filesystem::path dev = "/dev";
};

// Every real whole disk (no loop/ram/zram/optical/dm/md), with its verdict.
std::vector<DiskStatus> survey_disks(const SystemView& view = {});

// Only the eligible ones, sorted by name.
std::vector<Device> list_devices(const SystemView& view = {});

// The physical whole disks that hold `path` (through dm/md). Used to refuse
// writing an image onto the drive it is read from. Empty when unknown.
std::vector<std::string> disks_holding_path(const std::filesystem::path& path,
                                            const SystemView& view = {});

// Unmounts everything on the drive (its partitions, through dm/md on Linux).
// Calls `on_unmounted` per mount point; false with `error` set when one refuses.
// Windows: each volume is locked and dismounted (needs an administrator); its
// drive letter stays, and opening the drive for writing does it again and
// keeps the locks for as long as the drive is open.
bool unmount_device(const Device& d, std::string* error,
                    const std::function<void(const std::string&)>& on_unmounted = {});

// A drive as the user names it: "/dev/sdc" or "sdc"; "\\.\PhysicalDrive2" or
// "PhysicalDrive2". Returns the name (Device::name).
std::string device_name_from_arg(const std::string& arg);

std::string format_size(uint64_t bytes);  // "14.9 GiB"

}  // namespace kiln
