#pragma once

#include <stdexcept>
#include <string>

namespace tx {

/// Base class for every error thrown by TorrentX.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message)
        : std::runtime_error(message) {}
};

/// Malformed bencode data.
class BencodeError : public Error {
public:
    using Error::Error;
};

/// Structurally invalid .torrent metadata.
class TorrentError : public Error {
public:
    using Error::Error;
};

/// Tracker unreachable or returned a failure.
class TrackerError : public Error {
public:
    using Error::Error;
};

/// Socket-level failure (connect, send, recv).
class NetworkError : public Error {
public:
    using Error::Error;
};

/// Receive timed out with no data consumed — benign for an idle peer,
/// unlike other NetworkErrors which mean the connection is unusable.
class TimeoutError : public NetworkError {
public:
    using NetworkError::NetworkError;
};

/// Peer violated the BitTorrent wire protocol.
class ProtocolError : public Error {
public:
    using Error::Error;
};

/// Local filesystem failure while reading/writing payload data.
class StorageError : public Error {
public:
    using Error::Error;
};

} // namespace tx
