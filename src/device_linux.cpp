// device_linux.cpp — disk survey from sysfs, mountinfo and /proc/swaps.
//
// Mounts are matched by their SOURCE path, not by the major:minor in
// mountinfo: btrfs reports an anonymous 0:N device there, so on a btrfs root
// the numbers would match nothing and the root disk would look unused.
#include "device.hpp"

#include <sys/mount.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace kiln {
namespace {

namespace fs = std::filesystem;

std::string read_line(const fs::path& p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t'))
        s.pop_back();
    size_t start = s.find_first_not_of(' ');
    return start == std::string::npos ? std::string() : s.substr(start);
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

// mountinfo and /proc/swaps escape space, tab, newline and backslash as \ooo.
std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 2])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
            out += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<Mount> read_mounts(const fs::path& mountinfo) {
    std::vector<Mount> mounts;
    std::ifstream f(mountinfo);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::vector<std::string> fields;
        for (std::string w; in >> w;) fields.push_back(w);
        // id parent major:minor root mountpoint options [optional...] - fstype source superopts
        auto sep = std::find(fields.begin(), fields.end(), "-");
        if (fields.size() < 5 || sep == fields.end() || sep + 2 >= fields.end()) continue;
        mounts.push_back({unescape(*(sep + 2)), unescape(fields[4])});
    }
    return mounts;
}

std::vector<std::string> read_swap_sources(const fs::path& swaps) {
    std::vector<std::string> out;
    std::ifstream f(swaps);
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string name, type;
        if (in >> name >> type && type == "partition") out.push_back(unescape(name));
        // Swap FILES live on a mounted filesystem, whose disk the mount check covers.
    }
    return out;
}

bool is_partition(const SystemView& v, const std::string& name) {
    return fs::exists(v.sys_block / name / "partition");
}

// "/dev/mapper/root" -> "dm-0", "/dev/sdc1" -> "sdc1"; "" for non-block sources
// (tmpfs, proc, server:/export, ...).
std::string block_name_for_source(const SystemView& v, const std::string& source) {
    if (!starts_with(source, "/dev/")) return {};
    const fs::path p = v.dev / source.substr(5);
    std::error_code ec;
    const fs::path real = fs::canonical(p, ec);
    std::string name = ec ? p.filename().string() : real.filename().string();
    if (name.empty() || !fs::exists(v.sys_block / name)) return {};
    return name;
}

// Partition -> its disk; dm/md -> the disks under them (via slaves/), recursively.
void physical_disks(const SystemView& v, const std::string& name, std::set<std::string>& out, int depth = 0) {
    if (depth > 16 || name.empty()) return;
    std::string disk = name;
    if (is_partition(v, name)) {
        std::error_code ec;
        const fs::path real = fs::canonical(v.sys_block / name, ec);
        if (ec) return;
        disk = real.parent_path().filename().string();
    }
    bool has_slaves = false;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(v.sys_block / disk / "slaves", ec)) {
        has_slaves = true;
        physical_disks(v, e.path().filename().string(), out, depth + 1);
    }
    if (!has_slaves) out.insert(disk);
}

// A mount here makes its disk a system disk. Prefix matches cover nested
// mounts (/boot/efi, /var/lib/docker, ...); anything else (a stick under
// /run/media) is merely "mounted" and can be unmounted on request.
bool is_system_mountpoint(const std::string& mp) {
    static const std::array<const char*, 14> roots = {
        "/", "/boot", "/efi", "/home", "/usr", "/var", "/opt", "/srv",
        "/nix", "/gnu", "/root", "/etc", "/sysroot", "/snap"};
    for (const char* r : roots) {
        const std::string root = r;
        if (mp == root) return true;
        if (root != "/" && starts_with(mp, root + "/")) return true;
    }
    return false;
}

// Names of things that are never a physical disk to flash.
bool is_virtual_name(const std::string& n) {
    static const std::array<const char*, 10> prefixes = {
        "loop", "ram", "zram", "sr", "dm-", "md", "nbd", "fd", "zd", "drbd"};
    for (const char* p : prefixes)
        if (starts_with(n, p)) return true;
    return false;
}

bool on_usb(const SystemView& v, const std::string& name) {
    std::error_code ec;
    const fs::path real = fs::canonical(v.sys_block / name, ec);
    if (ec) return false;
    for (const auto& part : real)
        if (starts_with(part.string(), "usb")) return true;
    return false;
}

}  // namespace

std::vector<DiskStatus> survey_disks(const SystemView& v) {
    const std::vector<Mount> mounts = read_mounts(v.mountinfo);
    const std::vector<std::string> swaps = read_swap_sources(v.swaps);

    // disk name -> first reason it is a system disk
    std::vector<std::pair<std::string, std::string>> system_reasons;
    auto mark_system = [&](const std::string& source, const std::string& why) {
        std::set<std::string> disks;
        physical_disks(v, block_name_for_source(v, source), disks);
        for (const auto& d : disks) {
            const bool known = std::any_of(system_reasons.begin(), system_reasons.end(),
                                           [&](const auto& p) { return p.first == d; });
            if (!known) system_reasons.emplace_back(d, why);
        }
    };
    for (const auto& m : mounts)
        if (is_system_mountpoint(m.mountpoint)) mark_system(m.source, "holds " + m.mountpoint + " (" + m.source + ")");
    for (const auto& s : swaps) mark_system(s, "holds swap (" + s + ")");

    std::vector<DiskStatus> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(v.sys_block, ec)) {
        const std::string name = entry.path().filename().string();
        if (is_virtual_name(name) || is_partition(v, name)) continue;
        const fs::path base = v.sys_block / name;

        DiskStatus st;
        Device& d = st.device;
        d.name = name;
        d.path = (fs::path("/dev") / name).string();
        try {
            d.size_bytes = std::stoull(read_line(base / "size")) * 512;
        } catch (const std::exception&) {
            d.size_bytes = 0;
        }
        try {
            d.logical_block = static_cast<uint32_t>(std::stoul(read_line(base / "queue/logical_block_size")));
        } catch (const std::exception&) {
            d.logical_block = 512;
        }
        d.read_only = read_line(base / "ro") == "1";
        d.vendor = read_line(base / "device/vendor");
        if (d.vendor == "ATA") d.vendor.clear();  // libata's placeholder, not a maker
        d.model = read_line(base / "device/model");
        if (d.model.empty()) d.model = read_line(base / "device/name");  // mmc cards

        const bool usb = on_usb(v, name);
        const bool sd = starts_with(name, "mmcblk") && read_line(base / "device/type") == "SD";
        const bool removable = read_line(base / "removable") == "1";
        d.bus = usb ? Bus::usb : sd ? Bus::sd_card : removable ? Bus::removable : Bus::internal;

        for (const auto& m : mounts) {
            std::set<std::string> disks;
            physical_disks(v, block_name_for_source(v, m.source), disks);
            if (disks.count(name)) d.mounts.push_back(m);
        }

        auto sys = std::find_if(system_reasons.begin(), system_reasons.end(),
                                [&](const auto& p) { return p.first == name; });
        if (sys != system_reasons.end()) {
            st.eligibility = Eligibility::system_disk;
            st.reason = sys->second;
        } else if (!usb && !sd && !removable) {
            st.eligibility = Eligibility::fixed_disk;
            st.reason = "internal disk (not USB, not an SD card, not removable)";
        } else if (d.size_bytes == 0) {
            st.eligibility = Eligibility::no_media;
            st.reason = "no media";
        } else {
            st.eligibility = Eligibility::eligible;
        }
        out.push_back(std::move(st));
    }
    std::sort(out.begin(), out.end(), [](const DiskStatus& a, const DiskStatus& b) {
        return a.device.name < b.device.name;
    });
    return out;
}

std::vector<std::string> disks_holding_path(const fs::path& path, const SystemView& v) {
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(path, ec);
    if (ec) return {};
    const std::string target = real.string();

    // The longest mountpoint that is a prefix of the path is its filesystem.
    const Mount* best = nullptr;
    const std::vector<Mount> mounts = read_mounts(v.mountinfo);
    for (const auto& m : mounts) {
        const std::string& mp = m.mountpoint;
        const bool under = target == mp || mp == "/" ||
                           (starts_with(target, mp) && target.size() > mp.size() && target[mp.size()] == '/');
        if (under && (!best || mp.size() >= best->mountpoint.size())) best = &m;
    }
    if (!best) return {};
    std::set<std::string> disks;
    physical_disks(v, block_name_for_source(v, best->source), disks);
    return {disks.begin(), disks.end()};
}

bool unmount_device(const Device& d, std::string* error, const std::function<void(const std::string&)>& on_unmounted) {
    std::vector<Mount> mounts = d.mounts;
    // Deepest first, so nested mounts come off before their parents.
    std::sort(mounts.begin(), mounts.end(),
              [](const Mount& a, const Mount& b) { return a.mountpoint.size() > b.mountpoint.size(); });
    for (const auto& m : mounts) {
        if (::umount2(m.mountpoint.c_str(), 0) != 0) {
            if (error) *error = "could not unmount " + m.mountpoint + ": " + std::strerror(errno);
            return false;
        }
        if (on_unmounted) on_unmounted(m.mountpoint);
    }
    return true;
}

}  // namespace kiln
