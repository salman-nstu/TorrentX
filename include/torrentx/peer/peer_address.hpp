#pragma once

#include <cstdint>
#include <string>
#include <tuple>

namespace tx::peer {

/// Network address of a remote peer as reported by the tracker.
struct PeerAddress {
    std::string ip;    ///< IPv4 dotted quad or a host name.
    uint16_t port = 0;

    std::string endpoint() const { return ip + ":" + std::to_string(port); }

    bool operator==(const PeerAddress& other) const
    {
        return ip == other.ip && port == other.port;
    }

    bool operator<(const PeerAddress& other) const
    {
        return std::tie(ip, port) < std::tie(other.ip, other.port);
    }
};

} // namespace tx::peer
