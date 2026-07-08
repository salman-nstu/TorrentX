#pragma once

#include <cstdint>
#include <string>

namespace tx::net {

/// RAII wrapper around a blocking TCP client socket.
///
/// Works on POSIX systems (primary target) and Windows/Winsock. All
/// operations throw tx::NetworkError on failure; the destructor closes
/// the descriptor, so a connection can never leak.
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    /// Resolves `host` (IP literal or name) and connects with a timeout.
    void connect(const std::string& host, uint16_t port, int timeoutMs);

    /// Applies a receive timeout to subsequent recv calls.
    void setReceiveTimeout(int timeoutMs);

    /// Sends the whole buffer, looping over partial writes.
    void sendAll(const uint8_t* data, size_t size);

    /// Receives exactly `size` bytes, looping over partial reads.
    /// Throws NetworkError on timeout or if the peer closes early.
    void recvExactly(uint8_t* out, size_t size);

    bool isOpen() const { return m_fd != kInvalid; }
    void close() noexcept;

private:
    // Sized to hold either a POSIX fd or a Windows SOCKET.
    static constexpr intptr_t kInvalid = -1;
    intptr_t m_fd = kInvalid;
};

} // namespace tx::net
