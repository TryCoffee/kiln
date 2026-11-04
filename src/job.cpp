// job.cpp
#include "job.hpp"

#include "image.hpp"
#include "ventoy.hpp"
#include "windows.hpp"
#include "writer.hpp"

#include <algorithm>
#include <chrono>
#include <optional>

namespace kiln {
namespace {

using K = HelperEvent::Kind;

HelperEvent ev(K kind, std::string text = {}) {
    HelperEvent e;
    e.kind = kind;
    e.text = std::move(text);
    return e;
}

constexpr const char* kReplug = "The system did not pick up the new partitions: unplug the drive and plug it back in.";

// Progress, at most ten lines a second per stage (and every stage's end).
class Throttle {
public:
    explicit Throttle(const std::function<void(const HelperEvent&)>& emit) : emit_(emit) {}
    void operator()(const Progress& p) {
        const auto now = std::chrono::steady_clock::now();
        if (p.stage == last_stage_ && p.done < p.total && now - last_ < std::chrono::milliseconds(100)) return;
        last_ = now;
        last_stage_ = p.stage;
        HelperEvent e;
        e.kind = K::stage;
        e.progress = p;
        emit_(e);
    }

private:
    const std::function<void(const HelperEvent&)>& emit_;
    std::chrono::steady_clock::time_point last_{};
    Stage last_stage_ = Stage::hashing;
};

int run(HelperRequest& req, const JobIo& io) {
    auto fail = [&](const std::string& text) {
        io.on_event(ev(K::error, text));
        return 1;
    };
    std::string why;

    // The drive, surveyed now.
    const std::string name = device_name_from_arg(req.device);
    std::optional<Device> dev;
    for (const auto& d : list_devices())
        if (d.name == name) dev = d;
    if (!dev) {
        for (const auto& st : survey_disks())
            if (st.device.name == name) return fail(st.device.path + " is not offered: " + st.reason);
        return fail("no drive named " + req.device + " (unplugged?)");
    }
    if (dev->read_only) return fail(dev->path + " is read-only (a lock switch on the card or adapter?)");

    ImageInfo info;
    std::optional<WindowsIsoInfo> win;
    uint64_t need = 0;
    if (req.ventoy) {
        if (const std::string w = ventoy_label_problem(req.ventoy_setup.label); !w.empty()) return fail("Ventoy: " + w);
        need = ventoy_min_bytes(req.ventoy_setup.reserve_mib);
    } else {
        const auto holders = disks_holding_path(io.image);
        if (std::find(holders.begin(), holders.end(), dev->name) != holders.end())
            return fail("the image is stored on " + dev->path + " itself");
        info = inspect_image(io.image);
        if (!req.raw) win = windows_iso_info(info, io.image);
        if (req.custom.any() && !win) return fail("the setup tweaks only apply to a Windows install ISO");
        if (!win && !raw_write_boots(info.kind) && !req.force)
            return fail("this image would not boot written raw (no partition table)");
        need = win ? windows_stick_bytes_needed(*win) : info.size_bytes;
    }
    if (need > dev->size_bytes)
        return fail("this needs " + format_size(need) + "; " + dev->path + " has " + format_size(dev->size_bytes));
    if (!req.custom.local_account.empty()) {
        req.custom.local_account = sanitize_account_name(req.custom.local_account, &why);
        if (req.custom.local_account.empty()) return fail("local account: " + why);
    }
    // Unmounting and opening the drive are the only steps that need the
    // administrator (Windows helper: everything else runs as the user).
    std::optional<TargetSpec> opened;
    bool unmounted = true;
    const auto privileged = [&] {
        if (!dev->mounts.empty())
            unmounted = unmount_device(*dev, &why, [&](const std::string& mp) { io.on_event(ev(K::msg, "Unmounted " + mp)); });
        if (unmounted) opened = io.open_device ? io.open_device(*dev) : TargetSpec(dev->path);
    };
    if (io.as_admin)
        io.as_admin(privileged);
    else
        privileged();
    if (!unmounted) return fail(why);
    const TargetSpec target = *opened;

    Throttle throttle(io.on_event);
    // A built drive's outcome (Windows stick, Ventoy disk), as events.
    const auto finish = [&](const auto& r) {
        if (r.cancelled) {
            io.on_event(ev(K::cancelled));
            return 3;
        }
        if (r.mismatch) {
            io.on_event(ev(K::mismatch, *r.mismatch));
            return 2;
        }
        if (r.replug) io.on_event(ev(K::msg, kReplug));
        HelperEvent ok = ev(K::ok);
        ok.direct = r.direct;
        ok.write_seconds = r.write_seconds;
        ok.verify_seconds = r.verify_seconds;
        io.on_event(ok);
        return 0;
    };
    if (req.ventoy) {
        VentoyOptions o;
        o.setup = req.ventoy_setup;
        o.verify = req.verify;
        o.cancel = io.cancel;
        o.progress = [&](const Progress& p) { throttle(p); };
        return finish(write_ventoy_disk(target, o));
    }
    if (win) {
        WindowsOptions o;
        o.verify = req.verify;
        o.custom = req.custom;
        o.temp_dir = io.temp_dir;
        o.cancel = io.cancel;
        o.progress = [&](const Progress& p) { throttle(p); };
        return finish(write_windows_stick(io.image, target, o));
    }
    WriteOptions o;
    o.verify = req.verify;
    o.cancel = io.cancel;
    o.progress = [&](const Progress& p) { throttle(p); };
    const WriteResult r = write_image(io.image, target, o);
    if (r.cancelled) {
        io.on_event(ev(K::cancelled));
        return 3;
    }
    if (r.mismatch_at) {
        io.on_event(ev(K::mismatch, "the drive differs from the image at byte " + std::to_string(*r.mismatch_at)));
        return 2;
    }
    if (r.replug) io.on_event(ev(K::msg, kReplug));
    HelperEvent ok = ev(K::ok);
    ok.direct = r.direct;
    ok.write_seconds = r.write_seconds;
    ok.verify_seconds = r.verify_seconds;
    ok.sha256 = r.sha256;
    io.on_event(ok);
    return 0;
}

}  // namespace

int run_write_job(HelperRequest req, const JobIo& io) {
    try {
        return run(req, io);
    } catch (const std::exception& e) {
        io.on_event(ev(K::error, e.what()));
        return 1;
    }
}

}  // namespace kiln
