// main_windows.cpp — kiln-helper on Windows: the elevated writer behind the GUI.
//
// Started by the GUI through UAC (ShellExecuteEx, "runas") as
// `kiln-helper.exe --pipe NAME write <drive> <image> [options]` (or `ventoy
// <drive> [options]`). An elevated process's stdin/stdout do not reach the
// GUI, so events go to, and "cancel" comes from, the GUI's named pipe
// \\.\pipe\NAME (protocol: helper_protocol.hpp).
// It trusts nothing it is given:
//  - the job runs impersonating the token of the process at the other end of
//    the pipe (the GUI: the user, not elevated), so the image, the temp files
//    and every path are opened with the user's rights, never lent the
//    administrator's (the Linux helper's open_as_user, done for the whole job);
//    only unmounting and opening the drive step out of it (JobIo::as_admin);
//  - the drive must be one Kiln offers right now (run_write_job re-surveys:
//    never a system disk, a file, or the disk the image is on).
#include "blockio.hpp"
#include "helper_protocol.hpp"
#include "job.hpp"
#include "win32_util.hpp"

#include <windows.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace kiln;
using K = HelperEvent::Kind;

std::atomic<bool> g_cancel{false};

// The pipe, opened for overlapped I/O: a synchronous handle would serialise the
// reader thread's pending ReadFile with every event the job writes.
class Pipe {
public:
    explicit Pipe(HANDLE h) : h_(h) {}
    HANDLE handle() const { return h_.get(); }

    bool write_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(mu_);
        const std::string data = line + "\n";
        return io(false, const_cast<char*>(data.data()), static_cast<DWORD>(data.size())) == data.size();
    }
    // Bytes read, 0 when the GUI closed its end.
    DWORD read(char* buf, DWORD n) { return io(true, buf, n); }

private:
    DWORD io(bool reading, char* buf, DWORD n) {
        OVERLAPPED ov{};
        Fd ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ov.hEvent = ev.get();
        DWORD done = 0;
        const BOOL ok = reading ? ReadFile(h_.get(), buf, n, nullptr, &ov) : WriteFile(h_.get(), buf, n, nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return 0;
        if (!GetOverlappedResult(h_.get(), &ov, &done, TRUE)) return 0;
        return done;
    }

    Fd h_;
    std::mutex mu_;
};

std::unique_ptr<Pipe> g_pipe;

void emit(const HelperEvent& e) {
    const std::string line = format_event(e);
    if (!g_pipe || !g_pipe->write_line(line)) {
        if (g_pipe) g_cancel = true;  // the GUI went away: stop the write
        std::cout << line << std::endl;
    }
}

int fail(const std::string& text) {
    HelperEvent e;
    e.kind = K::error;
    e.text = text;
    emit(e);
    return 1;
}

// An impersonation token for the process that created the pipe (the GUI).
Fd gui_token(HANDLE pipe, std::string& why) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) {
        why = "cannot tell who started the writer: " + win::error_text(GetLastError());
        return Fd();
    }
    Fd proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE primary = nullptr;
    if (!proc.valid() || !OpenProcessToken(proc.get(), TOKEN_DUPLICATE | TOKEN_QUERY, &primary)) {
        why = "cannot read the user's identity from Kiln: " + win::error_text(GetLastError());
        return Fd();
    }
    Fd own(primary);
    HANDLE imp = nullptr;
    if (!DuplicateTokenEx(primary, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr, SecurityImpersonation, TokenImpersonation, &imp)) {
        why = "cannot take the user's identity: " + win::error_text(GetLastError());
        return Fd();
    }
    return Fd(imp);
}

int run(std::vector<std::string> args) {
    if (!running_as_admin()) return fail("kiln-helper must run as administrator (it is started by Kiln through UAC)");
    if (args.size() < 2 || args[0] != "--pipe") return fail("usage: kiln-helper --pipe NAME write <drive> <image> [options] | ventoy <drive> [options]");
    const std::wstring pipe_name = L"\\\\.\\pipe\\" + win::wide(args[1]);
    args.erase(args.begin(), args.begin() + 2);

    HANDLE h = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot reach Kiln's pipe " + args[1] + ": " + win::error_text(GetLastError()));
    g_pipe = std::make_unique<Pipe>(h);

    // "cancel", or the GUI closing the pipe, stops the write. Bounded: the line
    // comes from another process, and an endless one must not grow memory here.
    std::thread([] {
        std::string line;
        char c;
        while (g_pipe->read(&c, 1) == 1) {
            if (c == '\n') {
                if (line == "cancel") break;
                line.clear();
            } else if (line.size() < 16) {
                line += c;
            }
        }
        g_cancel = true;
    }).detach();

    HelperRequest req;
    try {
        req = parse_helper_args(args);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    std::string why;
    const Fd user = gui_token(g_pipe->handle(), why);
    if (!user.valid()) return fail(why);
    if (!SetThreadToken(nullptr, user.get())) return fail("cannot take the user's identity: " + win::error_text(GetLastError()));

    auto drive = std::make_shared<std::unique_ptr<Target>>();  // keeps the drive (and its volume locks) open for the job
    JobIo io;
    io.image = req.image;
    io.on_event = emit;
    io.cancel = &g_cancel;
    io.temp_dir = req.temp_dir;  // the user's: everything in it is written as the user
    io.as_admin = [&user](const std::function<void()>& f) {
        RevertToSelf();
        struct Back {
            HANDLE t;
            ~Back() {
                if (!SetThreadToken(nullptr, t)) std::abort();  // never continue with the administrator's rights
            }
        } back{user.get()};
        f();
    };
    io.open_device = [drive](const Device& d) {
        *drive = std::make_unique<Target>(open_target(d.path, Access::read_write));
        return TargetSpec::descriptor((*drive)->fd.get(), d.path);
    };
    const int rc = run_write_job(req, io);
    RevertToSelf();
    if (*drive) {
        const std::string path = (*drive)->name;
        drive->reset();                        // release the volume locks...
        reread_partition_table(TargetSpec(path));  // ...and let Windows mount the new layout
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    // argv is UTF-8: the manifest sets the process code page.
    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
