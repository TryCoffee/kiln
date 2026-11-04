// windows.cpp — build a Windows install stick: GPT + FAT32 + the ISO's files,
// install.wim split to .swm through the patched wimlib (read straight from the ISO).
#include "windows.hpp"

#include "blockio.hpp"
#include "fat.hpp"
#include "gpt.hpp"
#include "image.hpp"
#include "udf.hpp"

#include <hivex.h>
#include <wimlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <random>
#include <stdexcept>
#include <system_error>

namespace kiln {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr uint64_t kFat32MaxFile = 0xFFFFFFFFull;
constexpr uint64_t kSwmPartSize = 4094ull << 20;  // as Rufus: below FAT32's limit with room for headers
constexpr uint64_t kMiB = 1u << 20;

struct Cancelled {};

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// FAT volume label: 11 characters, upper case, no characters FatFs refuses.
std::string fat_label(const std::string& volume_id) {
    std::string l;
    for (char c : volume_id) {
        if (l.size() == 11) break;
        const unsigned char u = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(c)));
        l += (std::isalnum(u) || u == '_' || u == '-') ? static_cast<char>(u) : '_';
    }
    return l.empty() ? "WINDOWS" : l;
}

// wimlib's strings are wimlib_tchar: char on Linux/macOS, UTF-16 wchar_t on
// Windows. Kiln's are UTF-8 (on Windows the process code page, set by the
// manifest), so std::filesystem::path converts between the two.
using WimString = std::basic_string<wimlib_tchar>;
#ifdef _WIN32
#define WIMTEXT(s) L##s
#define wim_strstr std::wcsstr
#else
#define WIMTEXT(s) s
#define wim_strstr std::strstr
#endif

WimString to_wim(const std::string& utf8) { return fs::path(utf8).native(); }

std::string from_wim(const wimlib_tchar* s) { return s ? fs::path(WimString(s)).string() : std::string(); }

int wim_read(void* ctx, void* buf, size_t n, uint64_t offset) {
    try {
        static_cast<UdfImage::File*>(ctx)->read_at(buf, n, offset);
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}

struct Wim {
    WIMStruct* w = nullptr;
    ~Wim() {
        if (w) wimlib_free(w);
    }
};

std::string wim_error(int code) { return from_wim(wimlib_get_error_string(static_cast<wimlib_error_code>(code))); }

void open_wim(UdfImage::File& f, const std::string& name, Wim& wim, wimlib_progress_func_t fn, void* ctx) {
    const WimString label = to_wim("iso:/" + name);  // never a real path: wimlib only prints it
    const int r = wimlib_open_wim_from_reader(wim_read, &f, f.size(), label.c_str(), 0, &wim.w, fn, ctx);
    if (r) throw std::runtime_error("cannot open " + name + " in the ISO: " + wim_error(r));
}

class TempDir {
public:
    explicit TempDir(const fs::path& base) {
        std::random_device rd;
        for (int attempt = 0; attempt < 100; ++attempt) {
            char name[32];
            std::snprintf(name, sizeof name, "kiln-%08x", static_cast<unsigned>(rd()));
            std::error_code ec;
            if (fs::create_directory(base / name, ec)) {  // false: it exists (or failed, ec set)
                path_ = base / name;
                return;
            }
            if (ec) throw std::system_error(ec, "create a temporary directory in " + base.string());
        }
        throw std::runtime_error("cannot find a free temporary directory name in " + base.string());
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

uint64_t free_space(const fs::path& p) {
    std::error_code ec;
    const fs::space_info s = fs::space(p, ec);
    return ec ? 0 : s.available;
}

fs::path pick_temp_base(const fs::path& requested, uint64_t need) {
    std::vector<fs::path> candidates;
    if (!requested.empty()) {
        candidates.push_back(requested);
    } else {
        if (const char* e = std::getenv("KILN_TMPDIR")) candidates.emplace_back(e);
#ifndef _WIN32
        candidates.emplace_back("/var/tmp");
#endif
        std::error_code ec;
        const fs::path t = fs::temp_directory_path(ec);
        if (!ec) candidates.push_back(t);
    }
    for (const auto& c : candidates)
        if (free_space(c) >= need) return c;
    throw std::runtime_error("install.wim must be split first, which needs " + std::to_string(need / kMiB) +
                             " MiB of free space; " +
                             (requested.empty() ? std::string("none of KILN_TMPDIR, /var/tmp, the temp dir has it")
                                                : requested.string() + " does not have it") +
                             " (choose another with --temp-dir)");
}

struct SplitProgress {
    const WindowsOptions* o;
    uint64_t part_base = 0;
    uint64_t total = 0;
};

wimlib_progress_status split_progress(wimlib_progress_msg msg, wimlib_progress_info* info, void* ctx) {
    auto* s = static_cast<SplitProgress*>(ctx);
    if (msg == WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART || msg == WIMLIB_PROGRESS_MSG_SPLIT_END_PART) {
        s->part_base = info->split.completed_bytes;
        s->total = info->split.total_bytes;
        if (s->o->progress) s->o->progress({Stage::splitting, s->part_base, s->total});
    } else if (msg == WIMLIB_PROGRESS_MSG_WRITE_STREAMS && s->total) {
        const uint64_t done = std::min(s->total, s->part_base + info->write_streams.completed_compressed_bytes);
        if (s->o->progress) s->o->progress({Stage::splitting, done, s->total});
    }
    if (s->o->cancel && s->o->cancel->load()) return WIMLIB_PROGRESS_STATUS_ABORT;
    return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

// install.swm, install2.swm, ... in part order.
std::vector<fs::path> swm_parts(const fs::path& dir) {
    std::vector<std::pair<int, fs::path>> found;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.size() < 11 || n.rfind("install", 0) != 0 || n.substr(n.size() - 4) != ".swm") continue;
        const std::string num = n.substr(7, n.size() - 11);
        found.emplace_back(num.empty() ? 1 : std::stoi(num), e.path());
    }
    std::sort(found.begin(), found.end());
    std::vector<fs::path> out;
    for (auto& [i, p] : found) out.push_back(p);
    return out;
}

std::string parent_of(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// What ends up on the stick, and where each file's bytes come from.
struct Item {
    std::string dest;     // path on the FAT volume, no leading slash
    uint64_t size = 0;
    std::string iso_path; // from the ISO, or
    fs::path file;        // from a temp file (a .swm part)
};

class ItemReader {
public:
    ItemReader(const UdfImage& img, const Item& it) {
        if (!it.iso_path.empty()) {
            udf_ = img.open(it.iso_path);
        } else {
            fd_ = open_file(it.file, Access::read);
            name_ = it.file.string();
        }
    }
    void read(void* buf, size_t n, uint64_t off) {
        if (udf_) return udf_->read_at(buf, n, off);
        if (pread_all(fd_.get(), buf, n, off, name_, false) != n)
            throw std::runtime_error(name_ + " is shorter than expected");
    }

private:
    std::unique_ptr<UdfImage::File> udf_;
    Fd fd_;
    std::string name_;
};

// Setup\LabConfig BypassTPMCheck / BypassSecureBootCheck / BypassRAMCheck = 1
// in an offline SYSTEM hive (what Rufus writes through the Windows registry API).
void add_labconfig(const fs::path& hive_file) {
    hive_h* h = hivex_open(hive_file.string().c_str(), HIVEX_OPEN_WRITE);
    if (!h) throw_errno("open the SYSTEM hive from boot.wim");
    struct Close {
        hive_h* h;
        ~Close() { hivex_close(h); }
    } close{h};
    const hive_node_h setup = hivex_node_get_child(h, hivex_root(h), "Setup");
    if (!setup) throw std::runtime_error("boot.wim's SYSTEM hive has no Setup key");
    hive_node_h lab = hivex_node_get_child(h, setup, "LabConfig");
    if (!lab) lab = hivex_node_add_child(h, setup, "LabConfig");
    if (!lab) throw_errno("add Setup\\LabConfig to the SYSTEM hive");
    char one[4] = {1, 0, 0, 0};  // REG_DWORD 1, little-endian
    hive_set_value values[3] = {{const_cast<char*>("BypassTPMCheck"), hive_t_REG_DWORD, 4, one},
                                {const_cast<char*>("BypassSecureBootCheck"), hive_t_REG_DWORD, 4, one},
                                {const_cast<char*>("BypassRAMCheck"), hive_t_REG_DWORD, 4, one}};
    if (hivex_node_set_values(h, lab, 3, values, 0) != 0) throw_errno("set the LabConfig values");
    if (hivex_commit(h, nullptr, 0) != 0) throw_errno("save the SYSTEM hive");
}

// The Windows Setup image in boot.wim: the one named "...Setup...", else 2 (as on
// every Microsoft ISO), else 1.
int setup_image(WIMStruct* w) {
    wimlib_wim_info wi{};
    wimlib_get_wim_info(w, &wi);
    for (int i = 1; i <= static_cast<int>(wi.image_count); ++i) {
        const wimlib_tchar* n = wimlib_get_image_name(w, i);
        if (n && wim_strstr(n, WIMTEXT("Setup"))) return i;
    }
    return wi.image_count >= 2 ? 2 : 1;
}

// boot.wim with the hardware-check bypass in its Setup image's registry, written
// to `dir`. Unchanged files are copied from the ISO's boot.wim as they are.
fs::path bypass_boot_wim(const UdfImage& img, const std::string& boot_wim, const fs::path& dir) {
    auto f = img.open(boot_wim);
    Wim wim;
    open_wim(*f, boot_wim, wim, nullptr, nullptr);
    const int index = setup_image(wim.w);
    const fs::path hive_dir = dir / "hive";
    fs::create_directories(hive_dir);
    const wimlib_tchar* hive_in_wim = WIMTEXT("/Windows/System32/config/SYSTEM");
    // NO_ACLS: Windows would otherwise apply the WIM's security descriptors (and
    // later capture the temp file's); Linux and macOS never handle them.
    int rc = wimlib_extract_paths(wim.w, index, hive_dir.c_str(), &hive_in_wim, 1,
                                  WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE | WIMLIB_EXTRACT_FLAG_NO_ACLS);
    if (rc) throw std::runtime_error("extracting the SYSTEM hive from boot.wim: " + wim_error(rc));
    const fs::path hive = hive_dir / "SYSTEM";
    add_labconfig(hive);

    wimlib_update_command cmd{};
    cmd.op = WIMLIB_UPDATE_OP_ADD;
    WimString src = hive.native();
    WimString dst = hive_in_wim;
    cmd.add.fs_source_path = src.data();
    cmd.add.wim_target_path = dst.data();
    cmd.add.add_flags = WIMLIB_ADD_FLAG_NO_ACLS;
    rc = wimlib_update_image(wim.w, index, &cmd, 1, 0);
    if (rc) throw std::runtime_error("putting the SYSTEM hive back into boot.wim: " + wim_error(rc));
    const fs::path out = dir / "boot.wim";
    rc = wimlib_write(wim.w, out.c_str(), WIMLIB_ALL_IMAGES, 0, 0);
    if (rc) throw std::runtime_error("writing the new boot.wim: " + wim_error(rc));
    return out;
}

}  // namespace

WindowsIsoInfo inspect_windows_iso(const fs::path& iso) {
    UdfImage img(iso);
    WindowsIsoInfo info;
    // Microsoft's ISOs name themselves in the ISO 9660 descriptor
    // ("CCCOMA_X64FRE_PL-PL_DV9"); their UDF volume ID is a generic "UDF Volume".
    info.volume_id = inspect_image(iso).volume_label;
    if (info.volume_id.empty()) info.volume_id = img.volume_id();
    for (const auto& e : img.list()) {
        if (e.is_dir) continue;
        ++info.file_count;
        info.files_bytes += e.size;
        const std::string l = lower(e.path);
        if (l == "sources/install.wim" || l == "sources/install.esd") {
            info.install_image = e.path;
            info.install_image_size = e.size;
        } else if (e.size > kFat32MaxFile) {
            info.too_big.push_back(e.path);
        }
        if (l == "sources/boot.wim") info.boot_wim = e.path;
        if (l == "sources/appraiserres.dll") info.appraiserres = e.path;
        if (l == "efi/boot/bootx64.efi") info.efi_x64 = true;
        if (l == "efi/boot/bootaa64.efi") info.efi_arm64 = true;
    }
    info.is_windows = !info.install_image.empty() && (info.efi_x64 || info.efi_arm64);
    info.needs_split = info.install_image_size > kFat32MaxFile;
    if (!info.install_image.empty()) {
        auto f = img.open(info.install_image);
        Wim wim;
        open_wim(*f, info.install_image, wim, nullptr, nullptr);
        wimlib_wim_info wi{};
        wimlib_get_wim_info(wim.w, &wi);
        for (int i = 1; i <= static_cast<int>(wi.image_count); ++i) {
            const wimlib_tchar* n = wimlib_get_image_property(wim.w, i, WIMTEXT("DISPLAYNAME"));
            if (!n) n = wimlib_get_image_name(wim.w, i);
            info.editions.emplace_back(n && *n ? from_wim(n) : "(unnamed)");
        }
    }
    return info;
}

std::optional<WindowsIsoInfo> windows_iso_info(const ImageInfo& info, const fs::path& image) {
    if (info.kind != ImageKind::plain_iso || !info.udf) return std::nullopt;
    WindowsIsoInfo w = inspect_windows_iso(image);
    if (!w.is_windows) return std::nullopt;
    return w;
}

uint64_t windows_stick_bytes_needed(const WindowsIsoInfo& info) {
    // Every file may waste up to one 32 KiB cluster; FATs, GPT and alignment fit in 64 MiB.
    return info.files_bytes + info.file_count * 32768 + 64 * kMiB;
}

WindowsResult write_windows_stick(const fs::path& iso, const TargetSpec& target, const WindowsOptions& o) {
    WindowsResult r;
    auto report = [&](Stage s, uint64_t done, uint64_t total) {
        if (o.progress) o.progress({s, done, total});
    };
    auto check_cancel = [&] {
        if (o.cancel && o.cancel->load(std::memory_order_relaxed)) throw Cancelled{};
    };

    const WindowsIsoInfo info = inspect_windows_iso(iso);
    if (!info.is_windows)
        throw std::runtime_error(iso.string() + " is not a Windows install ISO (no sources/install.wim or .esd "
                                                "with an EFI boot loader)");
    if (!info.too_big.empty())
        throw std::runtime_error(info.too_big.front() + " is 4 GiB or larger and FAT32 cannot hold it");
    if (info.needs_split && lower(info.install_image).ends_with(".esd"))
        throw std::runtime_error("install.esd is 4 GiB or larger; splitting an ESD is not supported yet");

    UdfImage img(iso);
    const auto entries = img.list();
    const auto start = Clock::now();
    bool reread = false;

    try {
        // 1. The target, checked before anything slow happens. Held open (O_EXCL on a
        //    drive) from here on, so nothing can mount it in the meantime.
        Target t = open_target(target, Access::read_write, /*direct_on_files=*/false, /*create=*/false);
        r.direct = t.direct;
        if (!t.block && t.size == 0)
            throw std::runtime_error("an image-file target must already have the stick's size "
                                     "(e.g. truncate -s 16G " + t.name + ")");
        const uint64_t need = windows_stick_bytes_needed(info);
        if (t.size < need)
            throw std::runtime_error("this ISO needs a drive of at least " + std::to_string(need / kMiB) +
                                     " MiB; " + t.name + " has " + std::to_string(t.size / kMiB) + " MiB");

        // 2. Everything that differs from the ISO is prepared before the drive is
        //    touched: the split parts, the patched boot.wim, the answer file.
        std::unique_ptr<TempDir> tmp;
        if (info.needs_split || o.custom.any()) {
            uint64_t temp_need = 64 * kMiB;
            if (info.needs_split) temp_need += info.install_image_size + 256 * kMiB;
            if (o.custom.bypass_checks && !info.boot_wim.empty()) temp_need += 2 * img.open(info.boot_wim)->size();
            tmp = std::make_unique<TempDir>(pick_temp_base(o.temp_dir, temp_need));
        }
        // Generated files, keyed by destination (lower case); they replace the
        // ISO's file of the same name, or are added.
        std::vector<Item> generated;
        if (o.custom.bypass_checks) {
            if (info.boot_wim.empty()) throw std::runtime_error("the ISO has no sources/boot.wim to apply the bypass to");
            report(Stage::customizing, 0, 1);
            const fs::path bw = bypass_boot_wim(img, info.boot_wim, tmp->path());
            generated.push_back({info.boot_wim, fs::file_size(bw), {}, bw});
            // As Rufus: an empty appraiserres.dll stops the compatibility check of an
            // upgrade started from a running Windows (original kept as .bak).
            if (!info.appraiserres.empty()) {
                const fs::path empty = tmp->path() / "appraiserres.dll";
                open_file(empty, Access::write, /*truncate=*/true);
                generated.push_back({info.appraiserres, 0, {}, empty});
                const std::string bak = info.appraiserres.substr(0, info.appraiserres.size() - 4) + ".bak";
                generated.push_back({bak, img.open(info.appraiserres)->size(), info.appraiserres, {}});
            }
            report(Stage::customizing, 1, 1);
        }
        if (o.custom.needs_answer_file()) {
            const std::string xml = unattend_xml(o.custom, info.efi_x64 ? "amd64" : "arm64");
            const fs::path u = tmp->path() / "unattend.xml";
            {
                Fd fd = open_file(u, Access::write, /*truncate=*/true);
                pwrite_all(fd.get(), xml.data(), xml.size(), 0, u.string());
            }
            // Setup copies sources/$OEM$/$$ into %WINDIR%, so the answer file lands in
            // %WINDIR%\Panther for the specialize and oobeSystem passes. (A root
            // Autounattend.xml without a windowsPE pass is not carried over: rufus#1981.)
            const std::string sources = parent_of(info.install_image);
            generated.push_back({(sources.empty() ? "" : sources + "/") + "$OEM$/$$/Panther/unattend.xml", xml.size(), {}, u});
        }

        std::vector<fs::path> parts;
        if (info.needs_split) {
            auto f = img.open(info.install_image);
            SplitProgress sp{&o};
            Wim wim;
            open_wim(*f, info.install_image, wim, split_progress, &sp);
            const int rc = wimlib_split(wim.w, (tmp->path() / "install.swm").c_str(), kSwmPartSize, 0);
            if (rc == WIMLIB_ERR_ABORTED_BY_PROGRESS) throw Cancelled{};
            if (rc) throw std::runtime_error("splitting " + info.install_image + ": " + wim_error(rc));
            parts = swm_parts(tmp->path());
            if (parts.empty()) throw std::runtime_error("wimlib wrote no .swm parts");
            for (const auto& p : parts)
                if (fs::file_size(p) > kFat32MaxFile)
                    throw std::runtime_error(p.filename().string() + " came out larger than FAT32 allows");
        }

        std::vector<Item> items;
        std::vector<std::string> dirs;
        uint64_t total = 0;
        auto generated_for = [&](const std::string& path) -> const Item* {
            for (const auto& g : generated)
                if (lower(g.dest) == lower(path)) return &g;
            return nullptr;
        };
        for (const auto& e : entries) {
            if (e.is_dir) {
                dirs.push_back(e.path);
            } else if (!(info.needs_split && e.path == info.install_image) && !generated_for(e.path)) {
                items.push_back({e.path, e.size, e.path, {}});
                total += e.size;
            }
        }
        for (const auto& g : generated) {
            for (std::string d = parent_of(g.dest); !d.empty(); d = parent_of(d))
                if (std::none_of(dirs.begin(), dirs.end(), [&](const std::string& x) { return lower(x) == lower(d); }))
                    dirs.push_back(d);
            items.push_back(g);
            total += g.size;
        }
        // Parents before children.
        std::stable_sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) {
            return std::count(a.begin(), a.end(), '/') < std::count(b.begin(), b.end(), '/');
        });
        const std::string sources = parent_of(info.install_image);
        for (const auto& p : parts) {
            const std::string dest = (sources.empty() ? "" : sources + "/") + p.filename().string();
            items.push_back({dest, fs::file_size(p), {}, p});
            total += items.back().size;
        }

        // 3. The drive: wipe old tables at both ends, write the GPT, format.
        report(Stage::partitioning, 0, 1);
        {
            AlignedBuffer zero(kMiB);
            std::memset(zero.get(), 0, kMiB);
            pwrite_all(t.fd.get(), zero.get(), kMiB, 0, t.name);
            pwrite_all(t.fd.get(), zero.get(), kMiB, round_down(t.size - kMiB, t.sector), t.name);
            const GptLayout g = make_gpt(t.size, t.sector, "Kiln");
            AlignedBuffer head(g.head.size());
            std::memcpy(head.get(), g.head.data(), g.head.size());
            pwrite_all(t.fd.get(), head.get(), g.head.size(), 0, t.name);
            AlignedBuffer tail(g.tail.size());
            std::memcpy(tail.get(), g.tail.data(), g.tail.size());
            pwrite_all(t.fd.get(), tail.get(), g.tail.size(), g.tail_lba * g.sector, t.name);

            FatVolume::format(t, g.part_offset(), g.part_bytes(), fat_label(info.volume_id));
            report(Stage::partitioning, 1, 1);

            // 4. Copy.
            {
                FatVolume vol(t, g.part_offset(), g.part_bytes());
                for (const auto& d : dirs) vol.make_dir("/" + d);
                uint64_t done = 0;
                for (const auto& it : items) {
                    ItemReader src(img, it);
                    vol.write_file(
                        "/" + it.dest, it.size, [&](void* b, size_t n, uint64_t off) { src.read(b, n, off); },
                        [&](size_t n) {
                            done += n;
                            report(Stage::copying, done, total);
                            check_cancel();
                        });
                }
                report(Stage::syncing, 0, 1);
                vol.sync();
            }
            report(Stage::syncing, 1, 1);
            r.write_seconds = seconds_since(start);

            // 5. Verify every file through a fresh mount, reading the medium.
            if (o.verify) {
                const auto vstart = Clock::now();
                drop_cached(t);
                FatVolume vol(t, g.part_offset(), g.part_bytes());
                AlignedBuffer expect(4u << 20);
                uint64_t done = 0;
                struct Mismatch {};
                for (const auto& it : items) {
                    ItemReader src(img, it);
                    uint64_t off = 0;
                    try {
                        const uint64_t got = vol.read_file("/" + it.dest, [&](const unsigned char* data, size_t n) {
                            if (off + n > it.size) throw Mismatch{};
                            src.read(expect.get(), n, off);
                            const auto diff = std::mismatch(data, data + n, expect.get());
                            if (diff.first != data + n) {
                                off += static_cast<uint64_t>(diff.first - data);
                                throw Mismatch{};
                            }
                            off += n;
                            done += n;
                            report(Stage::verifying, done, total);
                            check_cancel();
                        });
                        if (got != it.size) throw Mismatch{};
                    } catch (const Mismatch&) {
                        r.mismatch = it.dest + " at byte " + std::to_string(off);
                        break;
                    }
                }
                r.verify_seconds = seconds_since(vstart);
            }
        }
        reread = t.block;
    } catch (const Cancelled&) {
        r.cancelled = true;
    }
    if (reread) r.replug = !reread_partition_table(target);  // after our O_EXCL descriptor is closed
    r.ok = !r.cancelled && !r.mismatch;
    return r;
}

}  // namespace kiln
