#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace tx::net { class TcpSocket; }

namespace tx::protocol {

/// Message ids of the BitTorrent peer wire protocol (BEP 3).
enum class MessageType : uint8_t {
    Choke         = 0,
    Unchoke       = 1,
    Interested    = 2,
    NotInterested = 3,
    Have          = 4,
    Bitfield      = 5,
    Request       = 6,
    Piece         = 7,
    Cancel        = 8,
    Port          = 9, // DHT extension; received but ignored.
};

/// One decoded wire message. Keep-alives are represented as std::nullopt
/// by readMessage() rather than as a Message.
struct Message {
    MessageType type = MessageType::Choke;
    std::vector<uint8_t> payload;
};

/// A `request`/`cancel` triple.
struct BlockRequest {
    uint32_t pieceIndex = 0;
    uint32_t begin = 0;
    uint32_t length = 0;
};

/// A received `piece` message: block data plus its position.
struct PieceBlock {
    uint32_t pieceIndex = 0;
    uint32_t begin = 0;
    std::vector<uint8_t> data;
};

/// Reads one length-prefixed message from the socket.
/// Returns std::nullopt for a keep-alive (length 0).
/// Throws NetworkError on socket failure, ProtocolError on oversized or
/// truncated frames.
std::optional<Message> readMessage(net::TcpSocket& socket);

/// Writes one message with its 4-byte big-endian length prefix.
void writeMessage(net::TcpSocket& socket, MessageType type,
                  const std::vector<uint8_t>& payload = {});

/// Writes a zero-length keep-alive frame.
void writeKeepAlive(net::TcpSocket& socket);

/// Payload builders / parsers for the messages TorrentX uses.
std::vector<uint8_t> encodeRequest(const BlockRequest& request);
BlockRequest decodeRequest(const Message& message);
PieceBlock decodePiece(Message&& message);
uint32_t decodeHave(const Message& message);

} // namespace tx::protocol
