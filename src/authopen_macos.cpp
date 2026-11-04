// authopen_macos.cpp — a drive descriptor from /usr/libexec/authopen.
//
// `authopen -stdoutpipe -o <flags> <path>` asks for the right
// sys.openfile.readwrite.<path> (an administrator approves it in macOS's own
// dialog), opens the path and sends the descriptor back over its stdout, which
// must be a UNIX socket, as SCM_RIGHTS ancillary data (authopen(1)).
#include "blockio.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

extern char** environ;

namespace kiln {

int authopen_device(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return -1;
    };
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return fail(std::string("socketpair: ") + std::strerror(errno));
    Fd ours(sv[0]), theirs(sv[1]);
    ::fcntl(ours.get(), F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, theirs.get(), STDOUT_FILENO);
    const std::string flags = std::to_string(O_RDWR);
    const char* argv[] = {"/usr/libexec/authopen", "-stdoutpipe", "-o", flags.c_str(), path.c_str(), nullptr};
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, argv[0], &fa, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return fail(std::string("cannot run authopen: ") + std::strerror(rc));
    theirs = Fd();  // only authopen holds the other end now: EOF when it exits

    char byte = 0;
    iovec iov{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    ssize_t got;
    do {
        got = ::recvmsg(ours.get(), &msg, 0);
    } while (got < 0 && errno == EINTR);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    int fd = -1;
    if (got > 0)
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) std::memcpy(&fd, CMSG_DATA(c), sizeof fd);
    if (fd < 0) {
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            return fail("not authorized to write " + path + " (authopen exit " + std::to_string(WEXITSTATUS(status)) + ")");
        return fail("authopen sent no descriptor for " + path);
    }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

}  // namespace kiln
