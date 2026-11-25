#include "clip_link.h"
#include "../utils/vine_utils.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vine {

namespace {
constexpr int64_t kRetryMs = 2000;
// ClipBridge's own limit is 512K chars; UTF-8 and escapes make that up to 4x the bytes
constexpr size_t kMaxLine = 4u << 20;
} // namespace

bool ClipLink::ensure_connected() {
    if (fd_ >= 0) return true;
    const int64_t now = monotonic_ms();
    if (now < next_try_ms_) return false;
    next_try_ms_ = now + kRetryMs;
    // Through a dir fd: the socket's full path does not fit in sun_path
    const int dir = open(dir_.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return false;
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "/proc/self/fd/%d/sock", dir);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    const bool ok = fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0;
    close(dir);
    if (!ok) {
        if (fd >= 0) close(fd);
        return false;
    }
    fd_ = fd;
    inbox_.clear();
    return true;
}

bool ClipLink::set(const std::string& text) {
    if (!ensure_connected()) return false;
    const std::string line = "set " + escape_line(text) + "\n";
    size_t off = 0;
    int stalls = 0;
    while (off < line.size()) {
        const ssize_t n = send(fd_, line.data() + off, line.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        // A large text can fill the socket buffer while the bridge reads; give it a moment
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++stalls < 200) {
            usleep(5000);
            continue;
        }
        close_link();
        return false;
    }
    return true;
}

std::vector<std::string> ClipLink::poll() {
    std::vector<std::string> out;
    if (!ensure_connected()) return out;
    char buf[16384];
    for (;;) {
        const ssize_t n = recv(fd_, buf, sizeof buf, 0);
        if (n > 0) {
            inbox_.append(buf, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        close_link(); // the bridge went away: reconnect later
        break;
    }
    size_t start = 0;
    for (size_t nl; (nl = inbox_.find('\n', start)) != std::string::npos; start = nl + 1) {
        const std::string line = inbox_.substr(start, nl - start);
        if (line.rfind("clip ", 0) == 0) out.push_back(unescape_line(line.substr(5)));
    }
    inbox_.erase(0, start);
    if (inbox_.size() > kMaxLine) inbox_.clear();
    return out;
}

void ClipLink::close_link() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}

} // namespace vine
