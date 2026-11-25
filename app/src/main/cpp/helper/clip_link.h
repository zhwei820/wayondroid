#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vine {

// Link to a rootless guest's clipboard bridge (ClipBridge.java) on <dir>/sock, never blocking: the bridge comes up
// after the guest boots and restarts with it, so the link reconnects on its own
class ClipLink {
public:
    explicit ClipLink(std::string dir) : dir_(std::move(dir)) {}
    ~ClipLink() { close_link(); }

    ClipLink(const ClipLink&) = delete;
    ClipLink& operator=(const ClipLink&) = delete;

    // The guest's clipboard becomes text; false when the bridge is not reachable
    bool set(const std::string& text);

    // Guest clipboard texts that arrived since the last call
    std::vector<std::string> poll();

    void close_link();

private:
    bool ensure_connected();

    std::string dir_;
    int fd_ = -1;
    int64_t next_try_ms_ = 0;
    std::string inbox_;
};

} // namespace vine
