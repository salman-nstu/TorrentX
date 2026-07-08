#include "torrentx/util/tcp_socket.hpp"

#include "torrentx/util/error.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace tx::net {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kNativeInvalid = INVALID_SOCKET;

// Winsock needs one-time process initialization.
struct WinsockInit {
    WinsockInit()
    {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() { WSACleanup(); }
};

void ensureNetworkingInitialized()
{
    static WinsockInit init;
}

int lastError() { return WSAGetLastError(); }
bool wouldBlock(int err) { return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS; }
bool isRecvTimeout(int err) { return err == WSAETIMEDOUT; }
void closeNative(NativeSocket s) { closesocket(s); }

void setNonBlocking(NativeSocket s, bool enabled)
{
    u_long mode = enabled ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
}
#else
using NativeSocket = int;
constexpr NativeSocket kNativeInvalid = -1;

void ensureNetworkingInitialized() {}
int lastError() { return errno; }
bool wouldBlock(int err) { return err == EINPROGRESS || err == EWOULDBLOCK; }
bool isRecvTimeout(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
void closeNative(NativeSocket s) { ::close(s); }

void setNonBlocking(NativeSocket s, bool enabled)
{
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

std::string errorText(int err)
{
#ifdef _WIN32
    char buffer[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(err),
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   buffer, sizeof(buffer), nullptr);
    std::string text = buffer;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text.empty() ? "error " + std::to_string(err) : text;
#else
    return std::strerror(err);
#endif
}

/// Waits until the socket is writable (connect finished) within timeoutMs.
bool waitWritable(NativeSocket s, int timeoutMs)
{
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(s, &writeSet);

    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    const int ready = select(static_cast<int>(s) + 1, nullptr, &writeSet, nullptr, &tv);
    return ready == 1;
}

} // namespace

TcpSocket::~TcpSocket()
{
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : m_fd(std::exchange(other.m_fd, kInvalid))
{
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
{
    if (this != &other) {
        close();
        m_fd = std::exchange(other.m_fd, kInvalid);
    }
    return *this;
}

void TcpSocket::close() noexcept
{
    if (m_fd != kInvalid) {
        closeNative(static_cast<NativeSocket>(m_fd));
        m_fd = kInvalid;
    }
}

void TcpSocket::connect(const std::string& host, uint16_t port, int timeoutMs)
{
    ensureNetworkingInitialized();
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results);
    if (rc != 0 || results == nullptr) {
        throw NetworkError("cannot resolve " + host);
    }

    std::string lastFailure = "no addresses";
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        NativeSocket s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kNativeInvalid) {
            lastFailure = errorText(lastError());
            continue;
        }

        // Non-blocking connect so we can enforce our own timeout.
        setNonBlocking(s, true);
        const int result = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool connected = (result == 0);
        if (!connected && wouldBlock(lastError())) {
            if (waitWritable(s, timeoutMs)) {
                int soError = 0;
#ifdef _WIN32
                int len = sizeof(soError);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
#else
                socklen_t len = sizeof(soError);
                getsockopt(s, SOL_SOCKET, SO_ERROR, &soError, &len);
#endif
                connected = (soError == 0);
                if (!connected) {
                    lastFailure = errorText(soError);
                }
            } else {
                lastFailure = "connect timed out";
            }
        } else if (!connected) {
            lastFailure = errorText(lastError());
        }

        if (connected) {
            setNonBlocking(s, false);
            m_fd = static_cast<intptr_t>(s);
            freeaddrinfo(results);
            return;
        }
        closeNative(s);
    }

    freeaddrinfo(results);
    throw NetworkError("connect to " + host + ":" + std::to_string(port) +
                       " failed: " + lastFailure);
}

void TcpSocket::setReceiveTimeout(int timeoutMs)
{
    if (!isOpen()) {
        throw NetworkError("setReceiveTimeout on closed socket");
    }
    const NativeSocket s = static_cast<NativeSocket>(m_fd);
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&value), sizeof(value));
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void TcpSocket::sendAll(const uint8_t* data, size_t size)
{
    if (!isOpen()) {
        throw NetworkError("send on closed socket");
    }
    const NativeSocket s = static_cast<NativeSocket>(m_fd);
    size_t sent = 0;
    while (sent < size) {
#ifdef _WIN32
        const int n = ::send(s, reinterpret_cast<const char*>(data + sent),
                             static_cast<int>(size - sent), 0);
#else
        const ssize_t n = ::send(s, data + sent, size - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            throw NetworkError("send failed: " + errorText(lastError()));
        }
        sent += static_cast<size_t>(n);
    }
}

void TcpSocket::recvExactly(uint8_t* out, size_t size)
{
    if (!isOpen()) {
        throw NetworkError("recv on closed socket");
    }
    const NativeSocket s = static_cast<NativeSocket>(m_fd);
    size_t received = 0;
    while (received < size) {
#ifdef _WIN32
        const int n = ::recv(s, reinterpret_cast<char*>(out + received),
                             static_cast<int>(size - received), 0);
#else
        const ssize_t n = ::recv(s, out + received, size - received, 0);
#endif
        if (n == 0) {
            throw NetworkError("connection closed by peer");
        }
        if (n < 0) {
            const int err = lastError();
            if (isRecvTimeout(err)) {
                // A timeout before the first byte of a frame is benign; a
                // timeout mid-frame leaves the stream desynchronized.
                if (received == 0) {
                    throw TimeoutError("recv timed out");
                }
                throw NetworkError("recv timed out mid-message");
            }
            throw NetworkError("recv failed: " + errorText(err));
        }
        received += static_cast<size_t>(n);
    }
}

} // namespace tx::net
