// device_macos.cpp — disk survey and unmount through DiskArbitration and IOKit.
//
// Same rule as on Linux: offered = an external USB drive or SD card (or other
// removable media), never internal, never a disk image or virtual disk, never
// the disk behind a system mount (/ or /System/Volumes/*), followed from an
// APFS container down to its physical store (a Mac booted from an external
// drive must not be offered that drive). Writes go to the raw node /dev/rdiskN.
#include "device.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>
#include <sys/mount.h>
#include <sys/param.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <regex>
#include <set>

namespace kiln {
namespace {

namespace fs = std::filesystem;

template <typename T>
struct CF {
    T ref = nullptr;
    explicit CF(T r = nullptr) : ref(r) {}
    ~CF() {
        if (ref) CFRelease(ref);
    }
    CF(const CF&) = delete;
    CF& operator=(const CF&) = delete;
};

std::string to_string(CFTypeRef v) {
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return {};
    char buf[512];
    if (!CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf, kCFStringEncodingUTF8)) return {};
    std::string s = buf;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    const auto first = s.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : s.substr(first);
}

bool to_bool(CFTypeRef v) { return v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(v)); }

uint64_t to_u64(CFTypeRef v) {
    long long n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberLongLongType, &n);
    return static_cast<uint64_t>(n);
}

// "/dev/disk3s1" or "disk3s1" -> "disk3"
std::string whole_name(std::string bsd) {
    if (bsd.rfind("/dev/", 0) == 0) bsd = bsd.substr(5);
    if (bsd.rfind("r", 0) == 0) bsd = bsd.substr(1);
    std::smatch m;
    static const std::regex re("^(disk[0-9]+)");
    return std::regex_search(bsd, m, re) ? m[1].str() : std::string();
}

// The physical whole disks under `bsd` (itself, or an APFS container's stores):
// walk up IOKit's service plane collecting whole IOMedia that are not `bsd`.
void physical_disks(const std::string& bsd, std::set<std::string>& out) {
    const std::string whole = whole_name(bsd);
    if (whole.empty()) return;
    out.insert(whole);
    io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, whole.c_str()));
    if (!media) return;
    io_iterator_t it = 0;
    if (IORegistryEntryCreateIterator(media, kIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents, &it) ==
        KERN_SUCCESS) {
        for (io_object_t e; (e = IOIteratorNext(it)) != 0; IOObjectRelease(e)) {
            if (!IOObjectConformsTo(e, kIOMediaClass)) continue;
            CF<CFTypeRef> name(IORegistryEntryCreateCFProperty(e, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0));
            const std::string n = whole_name(to_string(name.ref));
            if (!n.empty()) out.insert(n);
        }
        IOObjectRelease(it);
    }
    IOObjectRelease(media);
}

struct MountEntry {
    std::string source;      // "/dev/disk4s1"
    std::string mountpoint;
};

std::vector<MountEntry> mounts() {
    struct statfs* m = nullptr;
    const int n = getmntinfo(&m, MNT_NOWAIT);
    std::vector<MountEntry> out;
    for (int i = 0; i < n; ++i)
        if (std::strncmp(m[i].f_mntfromname, "/dev/disk", 9) == 0) out.push_back({m[i].f_mntfromname, m[i].f_mntonname});
    return out;
}

bool is_system_mountpoint(const std::string& mp) {
    return mp == "/" || mp.rfind("/System/Volumes/", 0) == 0 || mp == "/private/var/vm";
}

std::vector<std::string> whole_disk_names() {
    std::vector<std::string> out;
    DIR* d = opendir("/dev");
    if (!d) return out;
    static const std::regex re("^disk[0-9]+$");
    while (const dirent* e = readdir(d))
        if (std::regex_match(e->d_name, re)) out.emplace_back(e->d_name);
    closedir(d);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return std::stoi(a.substr(4)) < std::stoi(b.substr(4));
    });
    return out;
}

}  // namespace

std::vector<DiskStatus> survey_disks(const SystemView&) {
    CF<DASessionRef> session(DASessionCreate(kCFAllocatorDefault));
    if (!session.ref) return {};
    const std::vector<MountEntry> all_mounts = mounts();

    std::vector<std::pair<std::string, std::string>> system_reasons;  // disk -> why
    for (const auto& m : all_mounts)
        if (is_system_mountpoint(m.mountpoint)) {
            std::set<std::string> disks;
            physical_disks(m.source, disks);
            for (const auto& d : disks)
                if (std::none_of(system_reasons.begin(), system_reasons.end(), [&](const auto& p) { return p.first == d; }))
                    system_reasons.emplace_back(d, "holds " + m.mountpoint + " (" + m.source + ")");
        }

    std::vector<DiskStatus> out;
    for (const std::string& name : whole_disk_names()) {
        CF<DADiskRef> disk(DADiskCreateFromBSDName(kCFAllocatorDefault, session.ref, name.c_str()));
        if (!disk.ref) continue;
        CF<CFDictionaryRef> desc(DADiskCopyDescription(disk.ref));
        if (!desc.ref) continue;
        auto get = [&](CFStringRef key) { return CFDictionaryGetValue(desc.ref, key); };
        const std::string protocol = to_string(get(kDADiskDescriptionDeviceProtocolKey));
        const bool internal = to_bool(get(kDADiskDescriptionDeviceInternalKey));
        const bool removable = to_bool(get(kDADiskDescriptionMediaRemovableKey)) || to_bool(get(kDADiskDescriptionMediaEjectableKey));
        // Virtual: disk images, APFS/CoreStorage containers synthesised from other disks.
        const bool is_virtual = protocol == "Disk Image" || protocol == "Virtual Interface" || protocol.empty();

        DiskStatus st;
        Device& d = st.device;
        d.name = name;
        d.path = "/dev/r" + name;  // raw node: unbuffered and far faster than /dev/diskN
        d.vendor = to_string(get(kDADiskDescriptionDeviceVendorKey));
        d.model = to_string(get(kDADiskDescriptionDeviceModelKey));
        d.size_bytes = to_u64(get(kDADiskDescriptionMediaSizeKey));
        d.logical_block = static_cast<uint32_t>(to_u64(get(kDADiskDescriptionMediaBlockSizeKey)));
        if (d.logical_block == 0) d.logical_block = 512;
        d.read_only = !to_bool(get(kDADiskDescriptionMediaWritableKey));
        const bool usb = protocol == "USB";
        const bool sd = protocol == "Secure Digital" || protocol == "SD";
        d.bus = usb ? Bus::usb : sd ? Bus::sd_card : removable ? Bus::removable : Bus::internal;
        for (const auto& m : all_mounts)
            if (whole_name(m.source) == name) d.mounts.push_back({m.source, m.mountpoint});

        auto sys = std::find_if(system_reasons.begin(), system_reasons.end(), [&](const auto& p) { return p.first == name; });
        if (sys != system_reasons.end()) {
            st.eligibility = Eligibility::system_disk;
            st.reason = sys->second;
        } else if (is_virtual) {
            st.eligibility = Eligibility::fixed_disk;
            st.reason = "virtual disk (" + (protocol.empty() ? std::string("synthesized") : protocol) + ")";
        } else if (internal || (!usb && !sd && !removable)) {
            st.eligibility = Eligibility::fixed_disk;
            st.reason = "internal disk (" + protocol + ")";
        } else if (d.size_bytes == 0) {
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
    struct statfs s {};
    if (statfs(path.c_str(), &s) != 0) return {};
    std::set<std::string> disks;
    physical_disks(s.f_mntfromname, disks);
    return {disks.begin(), disks.end()};
}

bool unmount_device(const Device& d, std::string* error, const std::function<void(const std::string&)>& on_unmounted) {
    CF<DASessionRef> session(DASessionCreate(kCFAllocatorDefault));
    CF<DADiskRef> disk(session.ref ? DADiskCreateFromBSDName(kCFAllocatorDefault, session.ref, d.name.c_str()) : nullptr);
    if (!disk.ref) {
        if (error) *error = "cannot reach " + d.name + " through DiskArbitration";
        return false;
    }
    struct Result {
        bool done = false;
        std::string error;
    } result;
    DASessionScheduleWithRunLoop(session.ref, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    DADiskUnmount(
        disk.ref, kDADiskUnmountOptionWhole,
        [](DADiskRef, DADissenterRef dissenter, void* ctx) {
            auto* r = static_cast<Result*>(ctx);
            if (dissenter) {
                const std::string why = to_string(DADissenterGetStatusString(dissenter));
                char code[32];
                std::snprintf(code, sizeof code, "0x%x", static_cast<unsigned>(DADissenterGetStatus(dissenter)));
                r->error = "macOS refused to unmount: " + (why.empty() ? std::string(code) : why);
            }
            r->done = true;
        },
        &result);
    for (int i = 0; i < 300 && !result.done; ++i) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    DASessionUnscheduleFromRunLoop(session.ref, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    if (!result.done) result.error = "unmounting " + d.name + " did not finish within 30 s";
    if (!result.error.empty()) {
        if (error) *error = result.error;
        return false;
    }
    if (on_unmounted)
        for (const auto& m : d.mounts) on_unmounted(m.mountpoint);
    return true;
}

}  // namespace kiln
