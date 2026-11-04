// main.cpp — the `kiln` command line.
//
//   kiln list [--all]
//   kiln info <image> [--sha256]
//   kiln write <image> (<drive> | --to-file <path>) [--sha256 HEX] [--no-verify]
//              [--unmount] [--force] [--raw] [--temp-dir DIR] [--yes]
//              Windows setup tweaks: [--bypass-checks] [--no-ms-account] [--local-account NAME]
//              [--no-data-collection] [--no-bitlocker] [--declutter]
//
// A Windows ISO gets a stick built from its files (GPT + FAT32, install.wim
// split); anything else is written raw.
//   kiln verify <image> (<drive> | --to-file <path>)
//   kiln ventoy (<drive> | --to-file <path>) [--gpt] [--no-secure-boot] [--label NAME]
//               [--reserve MIB] [--no-verify] [--unmount] [--yes]
//
// Exit codes: 0 done, 1 error, 2 verification found a difference, 3 cancelled, 64 usage.
#include "device.hpp"
#include "blockio.hpp"
#include "image.hpp"
#include "sha256.hpp"
#include "ventoy.hpp"
#include "windows.hpp"
#include "writer.hpp"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace kiln;
using Clock = std::chrono::steady_clock;

constexpr int kExitError = 1;
constexpr int kExitMismatch = 2;
constexpr int kExitCancelled = 3;
constexpr int kExitUsage = 64;

std::atomic<bool> g_cancel{false};

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void on_sigint(int) {
    if (g_cancel.exchange(true)) std::_Exit(130);  // second Ctrl+C: leave now
}

int usage() {
    std::cerr << "usage:\n"
                 "  kiln list [--all]                 drives Kiln will write to (--all: every disk, and why not)\n"
                 "  kiln info <image> [--sha256]      what the image is and whether a raw write boots\n"
                 "  kiln write <image> <drive> [--sha256 HEX] [--no-verify] [--unmount] [--yes]\n"
                 "             Windows ISO: builds an install stick (GPT + FAT32); [--temp-dir DIR] for\n"
                 "             install.wim's split parts, [--raw] to write it raw instead\n"
                 "             setup tweaks (Windows ISOs, as Rufus):\n"
                 "               --bypass-checks       no TPM 2.0 / Secure Boot / 4 GB RAM requirement\n"
                 "               --no-ms-account       setup accepts a local account (no online one)\n"
                 "               --local-account NAME  create local admin NAME, empty password, changed at first logon\n"
                 "               --no-data-collection  privacy questions answered no, EULA page skipped\n"
                 "               --no-bitlocker        no automatic device encryption\n"
                 "               --declutter           OneDrive, Outlook, Teams, Copilot, news, ads off; classic menus\n"
                 "             other images: raw write; [--force] for one that will not boot\n"
                 "  kiln write <image> --to-file <path> [...]   (Windows mode: the file must already\n"
                 "             have the stick's size, e.g. truncate -s 16G stick.img)\n"
                 "  kiln verify <image> (<drive> | --to-file <path>)\n"
                 "  kiln ventoy <drive> [--gpt] [--no-secure-boot] [--label NAME] [--reserve MIB]\n"
                 "             [--no-verify] [--unmount] [--yes]\n"
                 "             makes the drive a Ventoy disk (Ventoy " << ventoy_version() << "): copy ISO files onto\n"
                 "             it afterwards and boot any of them from Ventoy's menu. MBR unless --gpt;\n"
                 "             --no-secure-boot leaves out Ventoy's shim; exFAT label NAME (default Ventoy);\n"
                 "             --reserve leaves MIB unpartitioned at the end\n"
                 "  kiln ventoy --to-file <path> [...]   (the file must already have the drive's size)\n"
                 "  drives: sdc or /dev/sdc (Linux), disk4 (macOS), PhysicalDrive2 (Windows)\n";
    return kExitUsage;
}

const char* bus_name(Bus b) {
    switch (b) {
        case Bus::usb: return "USB";
        case Bus::sd_card: return "SD card";
        case Bus::removable: return "removable";
        case Bus::internal: return "internal";
    }
    return "?";
}

std::string describe(const Device& d) {
    std::string label = d.vendor.empty() ? d.model : d.vendor + " " + d.model;
    if (label.empty()) label = "unnamed drive";
    return label + ", " + format_size(d.size_bytes) + ", " + bus_name(d.bus);
}

const char* stage_name(Stage s) {
    switch (s) {
        case Stage::hashing: return "Checking SHA-256";
        case Stage::writing: return "Writing";
        case Stage::syncing: return "Flushing";
        case Stage::verifying: return "Verifying";
        case Stage::splitting: return "Splitting install.wim";
        case Stage::customizing: return "Customizing setup";
        case Stage::partitioning: return "Partitioning";
        case Stage::copying: return "Copying files";
    }
    return "";
}

// One status line on stderr, redrawn at most five times a second.
class ProgressLine {
public:
    void operator()(const Progress& p) {
        const auto now = Clock::now();
        if (p.stage != stage_ || first_) {
            if (!first_) std::fputc('\n', stderr);
            first_ = false;
            stage_ = p.stage;
            stage_start_ = now;
            last_ = {};
        }
        const bool finished = p.done >= p.total;
        if (!finished && now - last_ < std::chrono::milliseconds(200)) return;
        last_ = now;
        if (p.stage == Stage::syncing || p.stage == Stage::partitioning || p.stage == Stage::customizing) {
            std::fprintf(stderr, "\r%s...%s", stage_name(p.stage), finished ? " done" : "");
        } else {
            const double secs = std::chrono::duration<double>(now - stage_start_).count();
            const double rate = secs > 0 ? static_cast<double>(p.done) / secs : 0;
            const int pct = p.total ? static_cast<int>(100.0 * static_cast<double>(p.done) / static_cast<double>(p.total)) : 0;
            std::fprintf(stderr, "\r%-21s %3d%%  %s / %s  %.1f MB/s   ", stage_name(p.stage), pct,
                         format_size(p.done).c_str(), format_size(p.total).c_str(), rate / 1e6);
        }
        std::fflush(stderr);
    }
    void end() {
        if (!first_) std::fputc('\n', stderr);
        first_ = true;
    }

private:
    bool first_ = true;
    Stage stage_ = Stage::writing;
    Clock::time_point stage_start_{};
    Clock::time_point last_{};
};

int cmd_list(bool all) {
    if (!all) {
        const auto devices = list_devices();
        if (devices.empty()) {
            std::cout << "No USB drives or SD cards found. (kiln list --all shows every disk and why it is left out.)\n";
            return 0;
        }
        for (const auto& d : devices) {
            std::cout << d.path << "  " << describe(d);
            if (d.read_only) std::cout << "  [read-only]";
            std::cout << "\n";
            for (const auto& m : d.mounts) std::cout << "    mounted: " << m.source << " on " << m.mountpoint << "\n";
        }
        return 0;
    }
    for (const auto& st : survey_disks()) {
        const char* verdict = st.eligibility == Eligibility::eligible      ? "OFFERED"
                              : st.eligibility == Eligibility::system_disk ? "system"
                              : st.eligibility == Eligibility::no_media    ? "empty"
                                                                           : "fixed";
        std::cout << st.device.path << "  " << describe(st.device) << "  [" << verdict << "]";
        if (!st.reason.empty()) std::cout << "  " << st.reason;
        std::cout << "\n";
    }
    return 0;
}

// SHA-256 of a file with a progress line; nullopt when cancelled.
std::optional<std::string> hash_file(const std::string& path, uint64_t size) {
    ProgressLine line;
    Sha256 h;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<unsigned char> buf(4u << 20);
    uint64_t done = 0;
    for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;) {
        h.update(buf.data(), n);
        done += n;
        line({Stage::hashing, done, size});
        if (g_cancel) {
            std::fclose(f);
            line.end();
            return std::nullopt;
        }
    }
    std::fclose(f);
    line.end();
    return h.hex();
}

void print_windows(const WindowsIsoInfo& w) {
    std::cout << "Windows:    install ISO (" << (w.efi_x64 ? "x64" : "") << (w.efi_x64 && w.efi_arm64 ? ", " : "")
              << (w.efi_arm64 ? "ARM64" : "") << " UEFI)\n"
              << "Install:    " << w.install_image << ", " << format_size(w.install_image_size)
              << (w.needs_split ? " (split into .swm parts for FAT32)" : "") << "\n"
              << "Editions:   " << w.editions.size() << "\n";
    for (size_t i = 0; i < w.editions.size(); ++i) std::cout << "    " << i + 1 << ". " << w.editions[i] << "\n";
    std::cout << "Stick:      at least " << format_size(windows_stick_bytes_needed(w)) << ", GPT + FAT32, UEFI boot\n";
    for (const auto& f : w.too_big) std::cout << "PROBLEM:    " << f << " is 4 GiB or larger: FAT32 cannot hold it\n";
}

int cmd_info(const std::string& image, bool want_sha) {
    const ImageInfo info = inspect_image(image);
    std::cout << "Image:      " << image << "\n"
              << "Size:       " << format_size(info.size_bytes) << " (" << info.size_bytes << " bytes)\n"
              << "Kind:       " << kind_name(info.kind) << "\n";
    if (!info.volume_label.empty()) std::cout << "Label:      " << info.volume_label << "\n";
    std::cout << "Layout:     " << (info.iso9660 ? "ISO 9660 " : "") << (info.udf ? "UDF " : "")
              << (info.mbr_signature ? "MBR " : "") << (info.gpt ? "GPT " : "") << "\n"
              << "Raw write:  " << (raw_write_boots(info.kind) ? "boots" : "will NOT boot") << "\n";
    if (const auto w = windows_iso_info(info, image)) print_windows(*w);
    if (want_sha) {
        const auto sha = hash_file(image, info.size_bytes);
        if (!sha) return kExitCancelled;
        std::cout << "SHA-256:    " << *sha << "\n";
    }
    return 0;
}

// Resolves a drive argument ("sdc" or "/dev/sdc") to an offered device, or
// explains why it is not one.
std::optional<Device> resolve_drive(const std::string& arg) {
    const std::string name = device_name_from_arg(arg);
    for (const auto& d : list_devices())
        if (d.name == name) return d;
    for (const auto& st : survey_disks())
        if (st.device.name == name) {
            std::cerr << "kiln: " << st.device.path << " is not offered: " << st.reason << "\n";
            return std::nullopt;
        }
    std::cerr << "kiln: no drive named " << arg << " (kiln list shows the drives Kiln will write to)\n";
    return std::nullopt;
}

struct WriteArgs {
    std::string image;
    std::string drive;
    std::string to_file;
    std::optional<std::string> sha256;
    bool verify = true;
    bool unmount = false;
    bool force = false;
    bool raw = false;
    bool yes = false;
    std::string temp_dir;
    Customization custom;
    VentoySetup ventoy;
    bool ventoy_options = false;  // any of --gpt/--no-secure-boot/--label/--reserve given
};

// The tweaks in words, for the summary line.
std::string describe(const Customization& c) {
    std::vector<std::string> v;
    if (c.bypass_checks) v.push_back("no TPM/Secure Boot/RAM checks");
    if (c.no_online_account) v.push_back("no Microsoft account needed");
    if (!c.local_account.empty()) v.push_back("local account '" + c.local_account + "'");
    if (c.no_data_collection) v.push_back("no data collection");
    if (c.no_bitlocker) v.push_back("no BitLocker");
    if (c.declutter) v.push_back("decluttered");
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i];
    return s;
}

int report(const WriteResult& r) {
    if (r.cancelled) {
        std::cerr << "Cancelled. The target now holds a partial image.\n";
        return kExitCancelled;
    }
    if (r.mismatch_at) {
        std::cerr << "VERIFY FAILED: the target differs from the image at byte " << *r.mismatch_at
                  << ". The drive may be failing or fake (smaller than it claims).\n";
        return kExitMismatch;
    }
    return 0;
}

// Picks the target (a drive passing every check, confirmed by the user, or
// --to-file). Fills target/what (and, on macOS run as a user, `authed`: the
// drive's descriptor from authopen); returns 0 or an exit code.
int choose_target(const WriteArgs& a, uint64_t need, std::string& target, std::string& what,
                  [[maybe_unused]] Fd& authed) {
    target = what = a.to_file;
    if (!target.empty()) return 0;
    const auto d = resolve_drive(a.drive);
    if (!d) return kExitError;
#if defined(_WIN32)
    if (!running_as_admin()) {
        std::cerr << "kiln: writing to a drive needs an administrator: run kiln in a terminal opened with "
                     "\"Run as administrator\"\n";
        return kExitError;
    }
#elif !defined(__APPLE__)  // macOS: no root needed, authopen asks for approval of this one drive
    if (::geteuid() != 0) {
        std::cerr << "kiln: writing to a drive needs root: run it with sudo\n";
        return kExitError;
    }
#endif
    if (d->read_only) {
        std::cerr << "kiln: " << d->path << " is read-only (a lock switch on the card or adapter?)\n";
        return kExitError;
    }
    const auto holders = a.image.empty() ? std::vector<std::string>{} : disks_holding_path(a.image);
    if (std::find(holders.begin(), holders.end(), d->name) != holders.end()) {
        std::cerr << "kiln: the image is stored on " << d->path << " itself\n";
        return kExitError;
    }
    if (need > d->size_bytes) {
        std::cerr << "kiln: this needs " << format_size(need) << "; " << d->path << " has "
                  << format_size(d->size_bytes) << "\n";
        return kExitError;
    }
    if (!d->mounts.empty() && !a.unmount) {
        std::cerr << "kiln: " << d->path << " is mounted:\n";
        for (const auto& m : d->mounts) std::cerr << "    " << m.source << " on " << m.mountpoint << "\n";
        std::cerr << "Unmount it first, or pass --unmount.\n";
        return kExitError;
    }
    what = d->path + " (" + describe(*d) + ")";
    if (!a.yes) {
#ifdef _WIN32
        const bool terminal = _isatty(_fileno(stdin));
#else
        const bool terminal = ::isatty(STDIN_FILENO);
#endif
        if (!terminal) {
            std::cerr << "kiln: confirmation needed: run it in a terminal, or pass --yes\n";
            return kExitError;
        }
        std::cerr << "This ERASES EVERYTHING on " << what << ".\nType " << d->name << " to continue: ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != d->name) {
            std::cerr << "Not confirmed; nothing was written.\n";
            return kExitError;
        }
    }
    std::string why;
    if (!d->mounts.empty() &&
        !unmount_device(*d, &why, [](const std::string& mp) { std::cerr << "Unmounted " << mp << "\n"; })) {
        std::cerr << "kiln: " << why << "\n";
        return kExitError;
    }
    target = d->path;
#ifdef __APPLE__
    if (::geteuid() != 0) {
        std::cerr << "macOS will ask for an administrator's approval to write " << d->path << ".\n";
        const int fd = authopen_device(d->path, &why);
        if (fd < 0) {
            std::cerr << "kiln: " << why << "\n";
            return kExitError;
        }
        authed = Fd(fd);
    }
#endif
    return 0;
}

std::string timing(double write_s, bool verified, double verify_s, bool direct) {
    return "written in " + std::to_string(static_cast<int>(write_s)) + " s" + (direct ? " (unbuffered)" : " (buffered)") +
           (verified ? ", verified in " + std::to_string(static_cast<int>(verify_s)) + " s" : ", not verified");
}

int write_windows(const WriteArgs& a, const ImageInfo& info, const WindowsIsoInfo& w) {
    if (!a.custom.local_account.empty()) {
        std::string why;
        const std::string name = sanitize_account_name(a.custom.local_account, &why);
        if (name.empty()) {
            std::cerr << "kiln: --local-account: " << why << "\n";
            return kExitError;
        }
        if (name != a.custom.local_account)
            std::cerr << "Local account name changed to '" << name << "' (characters Windows refuses)\n";
    }
    if (!w.too_big.empty()) {
        std::cerr << "kiln: " << w.too_big.front() << " is 4 GiB or larger and FAT32 cannot hold it\n";
        return kExitError;
    }
    std::string target, what;
    Fd authed;
    if (const int rc = choose_target(a, windows_stick_bytes_needed(w), target, what, authed)) return rc;
    if (a.sha256) {
        const auto sha = hash_file(a.image, info.size_bytes);
        if (!sha) return kExitCancelled;
        if (!same_digest(*sha, *a.sha256)) {
            std::cerr << "kiln: the image's SHA-256 is " << *sha << ", not the expected " << *a.sha256
                      << ": nothing was written\n";
            return kExitError;
        }
    }
    std::cerr << "Building a Windows install stick from " << a.image << " (" << w.editions.size()
              << " editions) on " << what << "\n"
              << "GPT + FAT32, UEFI boot" << (w.needs_split ? "; " + w.install_image + " split into .swm parts" : "")
              << "\n";
    if (a.custom.any()) std::cerr << "Setup: " << describe(a.custom) << "\n";
    ProgressLine line;
    WindowsOptions o;
    o.verify = a.verify;
    o.temp_dir = a.temp_dir;
    o.custom = a.custom;
    if (!o.custom.local_account.empty()) o.custom.local_account = sanitize_account_name(o.custom.local_account);
    o.cancel = &g_cancel;
    o.progress = [&line](const Progress& p) { line(p); };
    const WindowsResult r = write_windows_stick(
        a.image, authed.valid() ? TargetSpec::descriptor(authed.get(), target) : TargetSpec(target), o);
    line.end();
    if (r.cancelled) {
        std::cerr << "Cancelled. The drive is left incomplete.\n";
        return kExitCancelled;
    }
    if (r.mismatch) {
        std::cerr << "VERIFY FAILED: " << *r.mismatch << " differs from the ISO. "
                  << "The drive may be failing or fake (smaller than it claims).\n";
        return kExitMismatch;
    }
    std::cerr << "Done: " << timing(r.write_seconds, a.verify, r.verify_seconds, r.direct) << ".\n";
    if (r.replug) std::cerr << "The system did not pick up the new partitions: unplug the drive and plug it back in.\n";
    return 0;
}

int cmd_write(const WriteArgs& a) {
    const ImageInfo info = inspect_image(a.image);
    if (!a.raw)
        if (const auto w = windows_iso_info(info, a.image)) return write_windows(a, info, *w);
    if (a.custom.any()) {
        std::cerr << "kiln: the setup tweaks only apply to a Windows install ISO written as a built stick\n";
        return kExitError;
    }

    if (!raw_write_boots(info.kind) && !a.force) {
        if (info.kind == ImageKind::plain_iso)
            std::cerr << "kiln: " << a.image << " is a plain ISO with no partition table, and not a Windows\n"
                         "install ISO Kiln can build a stick from. Written raw it will not boot from a USB\n"
                         "stick. --force writes it raw anyway.\n";
        else
            std::cerr << "kiln: " << a.image << " has no partition table and no ISO 9660 layout, so a raw write\n"
                         "will not boot (is it compressed?). --force writes it anyway.\n";
        return kExitError;
    }

    std::string target, what;
    Fd authed;
    if (const int rc = choose_target(a, info.size_bytes, target, what, authed)) return rc;
    std::cerr << "Writing " << a.image << " (" << kind_name(info.kind) << ", " << format_size(info.size_bytes)
              << ") to " << what << "\n";
    ProgressLine line;
    WriteOptions o;
    o.verify = a.verify;
    o.expected_sha256 = a.sha256;
    o.cancel = &g_cancel;
    o.progress = [&line](const Progress& p) { line(p); };
    const WriteResult r =
        write_image(a.image, authed.valid() ? TargetSpec::descriptor(authed.get(), target) : TargetSpec(target), o);
    line.end();
    const int rc = report(r);
    if (rc != 0) return rc;
    std::cerr << "Done: " << timing(r.write_seconds, a.verify, r.verify_seconds, r.direct) << ". Image SHA-256 "
              << r.sha256 << "\n";
    if (r.replug) std::cerr << "The system did not pick up the new partitions: unplug the drive and plug it back in.\n";
    return 0;
}

int cmd_ventoy(const WriteArgs& a) {
    if (const std::string why = ventoy_label_problem(a.ventoy.label); !why.empty()) {
        std::cerr << "kiln: --label: " << why << "\n";
        return kExitError;
    }
    std::string target, what;
    Fd authed;
    if (const int rc = choose_target(a, ventoy_min_bytes(a.ventoy.reserve_mib), target, what, authed)) return rc;
    std::cerr << "Installing Ventoy " << ventoy_version() << " on " << what << "\n"
              << (a.ventoy.gpt ? "GPT" : "MBR") << ", exFAT data partition \"" << a.ventoy.label << "\", "
              << (a.ventoy.secure_boot ? "Secure Boot support" : "no Secure Boot support")
              << (a.ventoy.reserve_mib ? ", " + std::to_string(a.ventoy.reserve_mib) + " MiB left free at the end" : "")
              << "\n";
    ProgressLine line;
    VentoyOptions o;
    o.setup = a.ventoy;
    o.verify = a.verify;
    o.cancel = &g_cancel;
    o.progress = [&line](const Progress& p) { line(p); };
    const VentoyResult r =
        write_ventoy_disk(authed.valid() ? TargetSpec::descriptor(authed.get(), target) : TargetSpec(target), o);
    line.end();
    if (r.cancelled) {
        std::cerr << "Cancelled. The drive is left incomplete.\n";
        return kExitCancelled;
    }
    if (r.mismatch) {
        std::cerr << "VERIFY FAILED: " << *r.mismatch << " differs from what was written. "
                  << "The drive may be failing or fake (smaller than it claims).\n";
        return kExitMismatch;
    }
    std::cerr << "Done: " << timing(r.write_seconds, a.verify, r.verify_seconds, r.direct)
              << ". Copy ISO files onto the \"" << a.ventoy.label << "\" partition.\n";
    if (r.replug) std::cerr << "The system did not pick up the new partitions: unplug the drive and plug it back in.\n";
    return 0;
}

int cmd_verify(const std::string& image, const std::string& drive, const std::string& to_file) {
    std::string target = to_file;
    if (target.empty()) {
        const auto d = resolve_drive(drive);
        if (!d) return kExitError;
        target = d->path;
    }
    ProgressLine line;
    const WriteResult r = verify_image(image, target, &g_cancel, [&line](const Progress& p) { line(p); });
    line.end();
    const int rc = report(r);
    if (rc == 0) std::cerr << "Identical: " << target << " holds the image.\n";
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // the manifest makes Kiln's strings UTF-8; the console must agree
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);
#else
    struct sigaction sa {};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
#endif

    const std::string cmd = args[0];
    std::vector<std::string> pos;
    WriteArgs w;
    bool all = false, want_sha = false;
    try {
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string& s = args[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= args.size()) throw UsageError(s + " needs a value");
                return args[++i];
            };
            if (s == "--all") all = true;
            else if (s == "--sha256" && cmd == "info") want_sha = true;
            else if (s == "--sha256") w.sha256 = value();
            else if (s == "--to-file") w.to_file = value();
            else if (s == "--no-verify") w.verify = false;
            else if (s == "--unmount") w.unmount = true;
            else if (s == "--force") w.force = true;
            else if (s == "--raw") w.raw = true;
            else if (s == "--temp-dir") w.temp_dir = value();
            else if (s == "--bypass-checks") w.custom.bypass_checks = true;
            else if (s == "--no-ms-account") w.custom.no_online_account = true;
            else if (s == "--local-account") w.custom.local_account = value();
            else if (s == "--no-data-collection") w.custom.no_data_collection = true;
            else if (s == "--no-bitlocker") w.custom.no_bitlocker = true;
            else if (s == "--declutter") w.custom.declutter = true;
            else if (s == "--yes") w.yes = true;
            else if (s == "--gpt") w.ventoy.gpt = w.ventoy_options = true;
            else if (s == "--no-secure-boot") {
                w.ventoy.secure_boot = false;
                w.ventoy_options = true;
            } else if (s == "--label") {
                w.ventoy.label = value();
                w.ventoy_options = true;
            } else if (s == "--reserve") {
                const std::string v = value();
                if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos)
                    throw UsageError("--reserve takes a whole number of MiB");
                w.ventoy.reserve_mib = std::stoull(v);
                w.ventoy_options = true;
            }
            else if (s.rfind("--", 0) == 0) throw UsageError("unknown option " + s);
            else pos.push_back(s);
        }

        if (cmd == "--version" || cmd == "version") {
            std::cout << "kiln " << KILN_VERSION << "\n";
            return 0;
        }
        if (cmd == "list" && pos.empty()) return cmd_list(all);
        if (cmd == "info" && pos.size() == 1) return cmd_info(pos[0], want_sha);
        if (w.ventoy_options && cmd != "ventoy") throw UsageError("--gpt, --no-secure-boot, --label and --reserve are for kiln ventoy");
        if (cmd == "ventoy" && pos.size() + !w.to_file.empty() == 1) {
            if (!pos.empty()) w.drive = pos[0];
            return cmd_ventoy(w);
        }
        const bool one_target = (pos.size() == 2) != !w.to_file.empty();
        if (cmd == "write" && !pos.empty() && pos.size() <= 2 && one_target) {
            w.image = pos[0];
            if (pos.size() == 2) w.drive = pos[1];
            return cmd_write(w);
        }
        if (cmd == "verify" && !pos.empty() && pos.size() <= 2 && one_target)
            return cmd_verify(pos[0], pos.size() == 2 ? pos[1] : "", w.to_file);
        return usage();
    } catch (const UsageError& e) {
        std::cerr << "kiln: " << e.what() << "\n";
        return usage();
    } catch (const std::exception& e) {
        std::cerr << "\nkiln: " << e.what() << "\n";
        return kExitError;
    }
}
