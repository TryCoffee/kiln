// windows.hpp — Windows install sticks built from the ISO's files.
//
// A Windows ISO has no partition table, so a raw write does not boot. The
// stick is built instead: a GPT with one basic-data partition, FAT32 (FatFs,
// nothing mounted), every file from the ISO's UDF tree copied over, and
// sources/install.wim split into .swm parts below FAT32's 4 GiB file limit
// (Windows Setup reads split images natively). UEFI boot, Secure Boot intact,
// since the firmware boots the ISO's own signed efi/boot/boot*.efi.
#pragma once

#include "unattend.hpp"
#include "writer.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kiln {

struct WindowsIsoInfo {
    bool is_windows = false;       // UDF with sources/install.{wim,esd} and an EFI loader
    std::string volume_id;
    std::string install_image;     // "sources/install.wim" as spelled in the ISO
    uint64_t install_image_size = 0;
    bool needs_split = false;      // install image >= 4 GiB
    std::vector<std::string> editions;
    std::vector<std::string> too_big;  // other files >= 4 GiB: not possible on FAT32
    uint64_t files_bytes = 0;
    size_t file_count = 0;
    bool efi_x64 = false;
    bool efi_arm64 = false;
    std::string boot_wim;          // "sources/boot.wim" as spelled in the ISO
    std::string appraiserres;      // "sources/appraiserres.dll", if present
};

// Throws when the file is not a readable UDF image.
WindowsIsoInfo inspect_windows_iso(const std::filesystem::path& iso);

struct ImageInfo;
// The Windows details when `image` (already inspected as `info`) is a Windows
// install ISO Kiln builds sticks from; nullopt for anything else.
std::optional<WindowsIsoInfo> windows_iso_info(const ImageInfo& info, const std::filesystem::path& image);

// Bytes a stick needs for `info` (files + FAT overhead + table), a safe estimate.
uint64_t windows_stick_bytes_needed(const WindowsIsoInfo& info);

struct WindowsOptions {
    bool verify = true;
    // Where install.wim's split parts are written first (needs install image
    // size of free space); empty = $KILN_TMPDIR, else /var/tmp, else the temp dir.
    std::filesystem::path temp_dir;
    Customization custom;  // Rufus-style setup tweaks; all off = the ISO as it is
    const std::atomic<bool>* cancel = nullptr;
    ProgressFn progress;
};

struct WindowsResult {
    bool ok = false;
    bool cancelled = false;
    std::optional<std::string> mismatch;  // "<file> at byte N" when verify failed
    bool direct = false;
    bool replug = false;  // the OS did not take the new partition table: replug the drive
    double write_seconds = 0;
    double verify_seconds = 0;
};

WindowsResult write_windows_stick(const std::filesystem::path& iso, const TargetSpec& target,
                                  const WindowsOptions& options);

}  // namespace kiln
