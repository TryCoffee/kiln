// win32_util.cpp
#include "win32_util.hpp"

#include <winioctl.h>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace kiln::win {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string error_text(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string s = n ? utf8(std::wstring(buf, n)) : std::string();
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? "error " + std::to_string(code) : s + " (error " + std::to_string(code) + ")";
}

namespace {

// "\\?\Volume{...}\" -> "\\?\Volume{...}": CreateFile opens the volume itself
// only without the trailing backslash (with it: its root directory).
std::wstring device_path(const std::wstring& guid_path) {
    std::wstring p = guid_path;
    if (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

std::vector<DWORD> volume_disks(const std::wstring& guid_path) {
    HANDLE h = CreateFileW(device_path(guid_path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    Fd own(h);
    // Room for 32 extents: a volume spanning more disks than that is not on a stick.
    std::vector<unsigned char> buf(sizeof(VOLUME_DISK_EXTENTS) + 32 * sizeof(DISK_EXTENT));
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr))
        return {};  // no media, or not a disk-backed volume (a CD drive, a network share)
    const auto* e = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf.data());
    std::vector<DWORD> disks;
    for (DWORD i = 0; i < e->NumberOfDiskExtents; ++i) disks.push_back(e->Extents[i].DiskNumber);
    return disks;
}

}  // namespace

std::vector<Volume> list_volumes() {
    std::vector<Volume> out;
    wchar_t name[MAX_PATH];
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        Volume v;
        v.guid_path = name;
        v.disks = volume_disks(v.guid_path);
        // QueryDosDevice wants "Volume{...}": no "\\?\" and no trailing backslash.
        const std::wstring dos = device_path(v.guid_path).substr(4);
        wchar_t target[MAX_PATH];
        if (QueryDosDeviceW(dos.c_str(), target, MAX_PATH)) v.nt_device = target;
        DWORD len = 0;
        GetVolumePathNamesForVolumeNameW(v.guid_path.c_str(), nullptr, 0, &len);
        std::vector<wchar_t> paths(len + 1, L'\0');
        if (len && GetVolumePathNamesForVolumeNameW(v.guid_path.c_str(), paths.data(), len, &len))
            for (const wchar_t* p = paths.data(); *p; p += wcslen(p) + 1) v.mount_points.emplace_back(p);
        out.push_back(std::move(v));
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
    return out;
}

std::wstring volume_of_path(const std::wstring& path) {
    wchar_t mount[MAX_PATH];
    if (!GetVolumePathNameW(path.c_str(), mount, MAX_PATH)) return {};
    wchar_t guid[MAX_PATH];
    if (!GetVolumeNameForVolumeMountPointW(mount, guid, MAX_PATH)) return {};
    return guid;
}

std::optional<DWORD> disk_number(HANDLE disk) {
    STORAGE_DEVICE_NUMBER n{};
    DWORD got = 0;
    if (!DeviceIoControl(disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof n, &got, nullptr)) return std::nullopt;
    if (n.DeviceType != FILE_DEVICE_DISK) return std::nullopt;
    return n.DeviceNumber;
}

Fd lock_and_dismount(const Volume& v) {
    const std::string label = v.mount_points.empty() ? utf8(v.guid_path) : utf8(v.mount_points.front());
    HANDLE h = CreateFileW(device_path(v.guid_path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) throw std::runtime_error("unmounting " + label + " needs an administrator");
        throw std::runtime_error("cannot open the volume " + label + ": " + error_text(e));
    }
    Fd vol(h);
    DWORD got = 0;
    auto lock = [&] {
        for (int attempt = 0; attempt < 20; ++attempt) {  // ~2 s: Explorer and indexers let go quickly
            if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    };
    if (!lock()) {
        // Something keeps files open. The user has agreed to erase the drive:
        // a forced dismount invalidates those handles, then the lock is free.
        DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        if (!lock()) throw std::runtime_error("cannot lock " + label + ": " + error_text(GetLastError()));
    }
    if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr))
        throw std::runtime_error("cannot dismount " + label + ": " + error_text(GetLastError()));
    return vol;
}

}  // namespace kiln::win
