// job.hpp — one write, from a request to a result: checks, unmount, raw write,
// Windows stick or Ventoy disk, events. Run by kiln-helper on Linux (as root, after it has
// vetted the image) and on Windows (elevated, as the user except for the
// drive), and in-process on macOS (with an authopen descriptor).
#pragma once

#include "blockio.hpp"
#include "device.hpp"
#include "helper_protocol.hpp"

#include <atomic>
#include <functional>
#include <string>

namespace kiln {

struct JobIo {
    // The image to read: a path the caller has already vetted for the requester
    // (Linux helper: /proc/self/fd/N of a file opened with the user's identity).
    // Empty for a Ventoy disk.
    std::string image;
    std::function<void(const HelperEvent&)> on_event;  // not "emit": a Qt macro
    const std::atomic<bool>* cancel = nullptr;
    // How the drive is opened once it is chosen and unmounted; default: its path.
    // macOS: a descriptor from authopen; Windows helper: the drive opened (and
    // its volumes locked) as the administrator. Throw to refuse.
    std::function<TargetSpec(const Device&)> open_device;
    // Runs the unmount and open_device with the administrator's rights; unset:
    // they run as the process is. (Windows helper: the job impersonates the
    // user, and this steps out of it for the drive.)
    std::function<void(const std::function<void()>&)> as_admin;
    // Temporary files (split .swm parts, patched boot.wim); empty = default rules.
    std::string temp_dir;
};

// Emits stage/msg events and exactly one result event; returns the exit code
// (0 done, 1 error, 2 verify mismatch, 3 cancelled).
int run_write_job(HelperRequest req, const JobIo& io);

}  // namespace kiln
