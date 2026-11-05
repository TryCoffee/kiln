// main.cpp — kiln-helper: the privileged writer behind the GUI (Linux).
//
// Started by the GUI as `pkexec kiln-helper write <device> <image> [options]`,
// or `pkexec kiln-helper ventoy <device> [options]` (protocol in helper_protocol.hpp). It runs as root, so it trusts nothing it
// is given:
//  - the image is opened with the credentials of the user who asked
//    (PKEXEC_UID), so root's reading rights are never lent out: without that,
//    any user could have /etc/shadow written to a stick they own. Everything
//    after works on /proc/self/fd/N, the file that was checked;
//  - the drive must be one Kiln offers right now (run_write_job re-surveys:
//    never a system disk, a file, or the disk the image is on);
//  - temporary files only go where root puts them (/var/tmp), never a
//    caller-chosen directory.
// macOS needs none of this: Kiln runs as the user there and gets the drive
// from authopen.
#include "helper_protocol.hpp"
#include "job.hpp"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace kiln;
using K = HelperEvent::Kind;

std::atomic<bool> g_cancel{false};

void on_signal(int) { g_cancel = true; }

void emit(const HelperEvent& e) {
    std::cout << format_event(e) << std::endl;  // endl: the GUI reads line by line, live
}

int fail(const std::string& text) {
    HelperEvent e;
    e.kind = K::error;
    e.text = text;
    emit(e);
    return 1;
}

// The user on whose behalf we run: pkexec sets PKEXEC_UID, sudo SUDO_UID.
uid_t requesting_uid() {
    for (const char* var : {"PKEXEC_UID", "SUDO_UID"})
        if (const char* v = std::getenv(var)) {
            char* end = nullptr;
            const unsigned long u = std::strtoul(v, &end, 10);
            if (end && *end == '\0') return static_cast<uid_t>(u);
        }
    return ::getuid();
}

// open(path, O_RDONLY) with uid's identity (and its groups), then back to root.
int open_as_user(const std::string& path, uid_t uid, std::string& error) {
    if (uid == 0) return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    const passwd* pw = ::getpwuid(uid);
    if (!pw) {
        error = "unknown user id " + std::to_string(uid);
        return -1;
    }
    const gid_t gid = pw->pw_gid;
    int n = 64;
    std::vector<gid_t> groups(static_cast<size_t>(n));
    if (::getgrouplist(pw->pw_name, gid, groups.data(), &n) < 0) {
        groups.resize(static_cast<size_t>(n));
        ::getgrouplist(pw->pw_name, gid, groups.data(), &n);
    }
    groups.resize(static_cast<size_t>(n));
    const int root_ngroups = ::getgroups(0, nullptr);
    std::vector<gid_t> root_groups(static_cast<size_t>(std::max(root_ngroups, 0)));
    if (root_ngroups > 0) ::getgroups(root_ngroups, root_groups.data());

    if (::setgroups(groups.size(), groups.data()) != 0 || ::setegid(gid) != 0 || ::seteuid(uid) != 0) {
        error = std::string("cannot take the user's identity: ") + std::strerror(errno);
        return -1;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    const int open_errno = errno;
    // Back to root; if this fails, stop: nothing must run half-privileged.
    if (::seteuid(0) != 0 || ::setegid(0) != 0 || ::setgroups(root_groups.size(), root_groups.data()) != 0) std::abort();
    if (fd < 0) error = std::strerror(open_errno);
    return fd;
}

int run(const std::vector<std::string>& args) {
    if (::geteuid() != 0) return fail("kiln-helper must run as root (it is started through pkexec)");
    HelperRequest req;
    try {
        req = parse_helper_args(args);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    if (!req.temp_dir.empty()) return fail("--temp-dir is not accepted: as root, kiln-helper keeps its files in /var/tmp");
    JobIo io;
    if (!req.ventoy) {  // a Ventoy disk reads nothing of the user's
        std::string why;
        const int image_fd = open_as_user(req.image, requesting_uid(), why);
        if (image_fd < 0) return fail("cannot read " + req.image + ": " + why);
        io.image = "/proc/self/fd/" + std::to_string(image_fd);
    }
    io.on_event = emit;
    io.cancel = &g_cancel;
    io.temp_dir = "/var/tmp";
    return run_write_job(req, io);
}

}  // namespace

int main(int argc, char** argv) {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGINT, SIGTERM, SIGHUP}) ::sigaction(s, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);  // a closed GUI must not kill us mid-write; we cancel instead

    // "cancel" on stdin, or stdin closing (the GUI went away), stops the write.
    // So run by hand, it needs a live stdin: `sudo kiln-helper ... < /dev/null`
    // cancels at once. Read char by char with a bounded buffer: stdin belongs to
    // the caller, and an endless line must not grow memory in a root process.
    // Plain read(2), not std::cin: a thread blocked in std::cin holds stdin's
    // stdio lock, and glibc's exit() takes every stream's lock to flush them,
    // so the helper hung after its result until the GUI closed the pipe, which
    // the GUI only does once the helper has exited (Kiln 0.1.0).
    std::thread([] {
        std::string line;
        char buf[256];
        bool cancel = false;
        while (!cancel) {
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;  // a signal: on_signal has already set g_cancel
            if (n <= 0) break;
            for (ssize_t i = 0; i < n && !cancel; ++i) {
                if (buf[i] == '\n') {
                    cancel = line == "cancel";
                    line.clear();
                } else if (line.size() < 16) {
                    line += buf[i];
                }
            }
        }
        g_cancel = true;
    }).detach();

    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
