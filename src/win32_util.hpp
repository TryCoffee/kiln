// win32_util.hpp — what the Windows I/O and the drive survey share: text
// conversion, error messages, and the volumes on each disk.
#pragma once
#ifdef _WIN32

#include "blockio.hpp"

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace kiln::win {

std::wstring wide(const std::string& utf8);
std::string utf8(const std::wstring& w);
std::string error_text(DWORD code);  // FormatMessage, one line

struct Volume {
    std::wstring guid_path;                 // "\\?\Volume{...}\"
    std::wstring nt_device;                 // "\Device\HarddiskVolume3"
    std::vector<DWORD> disks;               // the physical disks it lives on
    std::vector<std::wstring> mount_points; // "E:\", "C:\mnt\stick\"
};

std::vector<Volume> list_volumes();

// The volume (as list_volumes() names it) a path lives on; empty if unknown.
std::wstring volume_of_path(const std::wstring& path);

// The physical disk number behind an open disk handle.
std::optional<DWORD> disk_number(HANDLE disk);

// Opens `guid_path`'s volume, locks it (retrying while something has files
// open; forcing a dismount when that never ends) and dismounts it. The lock
// lasts as long as the returned handle. Throws with a readable reason.
Fd lock_and_dismount(const Volume& v);

}  // namespace kiln::win

#endif
