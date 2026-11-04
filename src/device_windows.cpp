// device_windows.cpp — disk survey and unmount through the Win32 storage API.
//
// Same rule as on Linux and macOS: offered = a USB drive, an SD card, or other
// removable media; never a virtual disk (VHD/VHDX, Storage Spaces), never a
// fixed internal disk, and never a disk behind the system: the volume holding
// the Windows folder, the boot partition (HKLM\SYSTEM\Setup\SystemPartition) or
// any page file. Disks are \\.\PhysicalDriveN; their volumes are matched to
// them through IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, so a volume spanning
// several disks marks every one of them.
#include "device.hpp"

#include "win32_util.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <map>
#include <optional>

namespace kiln {
namespace {

namespace fs = std::filesystem;

// STORAGE_BUS_TYPE values, by number: older MinGW headers lack the newer names.
constexpr int kBusUsb = 7, kBusSd = 0xC, kBusMmc = 0xD, kBusVirtual = 0xE, kBusFileBackedVirtual = 0xF, kBusSpaces = 0x10;

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    const auto first = s.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : s.substr(first);
}

std::wstring reg_string(const wchar_t* key, const wchar_t* value, DWORD type_flags) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, type_flags, nullptr, nullptr, &size) != ERROR_SUCCESS) return {};
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, type_flags, nullptr, s.data(), &size) != ERROR_SUCCESS) return {};
    s.resize(size / sizeof(wchar_t));
    return s;
}

const win::Volume* find_volume(const std::vector<win::Volume>& vols, const std::wstring& guid) {
    for (const auto& v : vols)
        if (_wcsicmp(v.guid_path.c_str(), guid.c_str()) == 0) return &v;
    return nullptr;
}

// Disk number -> why it carries the running system.
std::map<DWORD, std::string> system_disks(const std::vector<win::Volume>& vols) {
    std::map<DWORD, std::string> out;
    auto mark = [&](const win::Volume* v, const std::string& why) {
        if (!v) return;
        for (DWORD d : v->disks) out.emplace(d, why);  // the first reason found is kept
    };
    wchar_t windir[MAX_PATH];
    if (GetWindowsDirectoryW(windir, MAX_PATH)) mark(find_volume(vols, win::volume_of_path(windir)), "holds " + win::utf8(windir));

    // "\Device\HarddiskVolume1": the partition the firmware boots (on UEFI, the ESP).
    const std::wstring sys_part = reg_string(L"SYSTEM\\Setup", L"SystemPartition", RRF_RT_REG_SZ);
    for (const auto& v : vols)
        if (!sys_part.empty() && _wcsicmp(v.nt_device.c_str(), sys_part.c_str()) == 0)
            mark(&v, "holds the boot partition (" + win::utf8(sys_part) + ")");

    // REG_MULTI_SZ of "\??\C:\pagefile.sys".
    const std::wstring pagefiles = reg_string(L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                                              L"ExistingPageFiles", RRF_RT_REG_MULTI_SZ);
    for (const wchar_t* p = pagefiles.c_str(); *p; p += wcslen(p) + 1) {
        std::wstring f = p;
        if (f.rfind(L"\\??\\", 0) == 0) f = f.substr(4);
        mark(find_volume(vols, win::volume_of_path(f)), "holds a page file (" + win::utf8(f) + ")");
    }
    return out;
}

struct Disk {
    bool exists = false;
    int bus = 0;
    bool removable = false;
    std::string vendor, model;
    uint64_t size = 0;
    uint32_t sector = 512;
    bool media = false;
    bool read_only = false;
};

Disk probe_disk(DWORD n) {
    Disk d;
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(n);
    // No access rights asked for: the queries below work for any user.
    Fd h(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h.valid()) return d;
    d.exists = true;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<unsigned char> buf(4096);
    DWORD got = 0;
    if (DeviceIoControl(h.get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) &&
        got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        d.bus = static_cast<int>(desc->BusType);
        d.removable = desc->RemovableMedia;
        auto str = [&](DWORD off) { return off && off < got ? trimmed(reinterpret_cast<const char*>(buf.data() + off)) : std::string(); };
        d.vendor = str(desc->VendorIdOffset);
        d.model = str(desc->ProductIdOffset);
    }
    DISK_GEOMETRY_EX g{};
    if (DeviceIoControl(h.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &g, sizeof g, &got, nullptr)) {
        d.media = true;
        d.size = static_cast<uint64_t>(g.DiskSize.QuadPart);
        d.sector = g.Geometry.BytesPerSector ? g.Geometry.BytesPerSector : 512;
    }
    if (d.media && !DeviceIoControl(h.get(), IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &got, nullptr))
        d.read_only = GetLastError() == ERROR_WRITE_PROTECT;
    return d;
}

std::optional<DWORD> number_of(const std::string& name) {
    const std::string prefix = "PhysicalDrive";
    if (name.size() <= prefix.size() || _strnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0) return std::nullopt;
    const std::string digits = name.substr(prefix.size());
    if (digits.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return static_cast<DWORD>(std::stoul(digits));
}

}  // namespace

std::vector<DiskStatus> survey_disks(const SystemView&) {
    const std::vector<win::Volume> vols = win::list_volumes();
    const std::map<DWORD, std::string> system = system_disks(vols);
    std::vector<DiskStatus> out;
    for (DWORD n = 0; n < 64; ++n) {  // numbers can have gaps after a drive is removed
        const Disk k = probe_disk(n);
        if (!k.exists) continue;
        DiskStatus st;
        Device& d = st.device;
        d.name = "PhysicalDrive" + std::to_string(n);
        d.path = "\\\\.\\" + d.name;
        d.vendor = k.vendor;
        d.model = k.model;
        d.size_bytes = k.size;
        d.logical_block = k.sector;
        d.read_only = k.read_only;
        // MMC is eMMC as often as a card: only a removable one counts as a card.
        d.bus = k.bus == kBusUsb                               ? Bus::usb
                : k.bus == kBusSd || (k.bus == kBusMmc && k.removable) ? Bus::sd_card
                : k.removable                                  ? Bus::removable
                                                               : Bus::internal;
        for (const auto& v : vols)
            if (std::find(v.disks.begin(), v.disks.end(), n) != v.disks.end())
                for (const auto& mp : v.mount_points) d.mounts.push_back({win::utf8(v.guid_path), win::utf8(mp)});

        const bool is_virtual = k.bus == kBusVirtual || k.bus == kBusFileBackedVirtual || k.bus == kBusSpaces;
        if (const auto s = system.find(n); s != system.end()) {
            st.eligibility = Eligibility::system_disk;
            st.reason = s->second;
        } else if (is_virtual) {
            st.eligibility = Eligibility::fixed_disk;
            st.reason = k.bus == kBusSpaces ? "Storage Spaces disk" : "virtual disk (VHD)";
        } else if (d.bus == Bus::internal) {
            st.eligibility = Eligibility::fixed_disk;
            st.reason = "internal disk";
        } else if (!k.media || k.size == 0) {
            st.eligibility = Eligibility::no_media;
            st.reason = "no media";
        } else {
            st.eligibility = Eligibility::eligible;
        }
        out.push_back(std::move(st));
    }
    return out;
}

std::vector<std::string> disks_holding_path(const fs::path& path, const SystemView&) {
    std::error_code ec;
    const fs::path full = fs::absolute(path, ec);
    const std::wstring guid = win::volume_of_path((ec ? path : full).native());
    if (guid.empty()) return {};
    std::vector<std::string> names;
    for (const auto& v : win::list_volumes())
        if (_wcsicmp(v.guid_path.c_str(), guid.c_str()) == 0)
            for (DWORD d : v.disks) names.push_back("PhysicalDrive" + std::to_string(d));
    return names;
}

bool unmount_device(const Device& d, std::string* error, const std::function<void(const std::string&)>& on_unmounted) {
    const auto n = number_of(d.name);
    if (!n) {
        if (error) *error = d.name + " is not a Windows drive name";
        return false;
    }
    try {
        for (const auto& v : win::list_volumes()) {
            if (std::find(v.disks.begin(), v.disks.end(), *n) == v.disks.end()) continue;
            win::lock_and_dismount(v);  // released at once: the drive's writing open locks again, and holds it
            if (on_unmounted)
                for (const auto& mp : v.mount_points) on_unmounted(win::utf8(mp));
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    return true;
}

}  // namespace kiln
