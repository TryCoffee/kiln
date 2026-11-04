// ventoy.hpp — Ventoy disks: Ventoy's boot loader plus an empty exFAT partition
// for ISO files, laid out as Ventoy's own installer lays them out (1.1.18,
// read from its source: INSTALL/tool/VentoyWorker.sh + ventoy_lib.sh,
// Ventoy2Disk/Ventoy2Disk/Utility.c + PhyDrive.c):
//
//   sector 0         Ventoy's boot code (GRUB boot.img), a random 16-byte disk ID
//                    at 0x180, a random disk signature, the partition table
//   sectors 1-2047   GRUB core.img, Ventoy's BIOS stage (GPT: sectors 34-2047)
//   partition 1      from 1 MiB: exFAT "Ventoy", where the ISO files go
//   partition 2      the last 32 MiB (4 KiB aligned): VTOYEFI, FAT16, Ventoy's
//                    EFI loaders and GRUB, written from Ventoy's image as it is
//
// Ventoy's three files (GPL-3.0) come from its official release, pinned by
// SHA-256 in CMakeLists.txt, and are built into Kiln (src/ventoy_write.cpp).
#pragma once

#include "blockio.hpp"
#include "writer.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace kiln {

// What the user picks (the CLI's options; the helper protocol carries it).
struct VentoySetup {
    bool gpt = false;          // MBR, Ventoy's default, or GPT
    bool secure_boot = true;   // Ventoy's shim + MokManager; false: its GRUB booted directly
    std::string label = "Ventoy";
    uint64_t reserve_mib = 0;  // left unpartitioned at the end of the drive
};

// Partition places in 512-byte sectors, inclusive.
struct VentoyLayout {
    uint64_t sectors = 0;
    uint64_t part1_first = 0, part1_last = 0;
    uint64_t part2_first = 0, part2_last = 0;
};

// Ventoy's installer's arithmetic; throws when the drive is too small, or too
// large for MBR (over 2^32 sectors).
VentoyLayout ventoy_layout(uint64_t disk_bytes, bool gpt, uint64_t reserve_mib);

// The smallest drive Kiln makes a Ventoy disk on (the data partition >= 64 MiB).
uint64_t ventoy_min_bytes(uint64_t reserve_mib = 0);

// Why `label` cannot name the exFAT partition, or empty when it can.
std::string ventoy_label_problem(const std::string& label);

const char* ventoy_version();  // "1.1.18"

struct VentoyOptions {
    VentoySetup setup;
    bool verify = true;
    const std::atomic<bool>* cancel = nullptr;
    ProgressFn progress;
};

struct VentoyResult {
    bool ok = false;
    bool cancelled = false;
    std::optional<std::string> mismatch;  // "<area> at byte N" when verify failed
    bool direct = false;
    bool replug = false;  // the OS did not take the new partition table: replug the drive
    double write_seconds = 0;
    double verify_seconds = 0;
};

// Erases the drive (or a pre-sized image file) and makes it a Ventoy disk.
VentoyResult write_ventoy_disk(const TargetSpec& target, const VentoyOptions& options);

}  // namespace kiln
