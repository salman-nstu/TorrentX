#include "torrentx/protocol/message.hpp"

#include "torrentx/util/byte_utils.hpp"
#include "torrentx/util/error.hpp"
#include "torrentx/util/tcp_socket.hpp"

namespace tx::protocol {

namespace {

// A piece message carries a 16 KiB block plus 9 bytes of header; a
// bitfield for a very large torrent can reach a few hundred KiB. Anything
// beyond 1 MiB is a misbehaving peer, not a valid frame.
constexpr uint32_t kMaxMessageSize = 1024 * 1024;

void requirePayloadSize(const Message& message, size_t expected, const char* what)
{
    if (message.payload.size() != expected) {
        throw ProtocolError(std::string("message: bad ") + what + " payload size");
    }
}

} // namespace

std::optional<Message> readMessage(net::TcpSocket& socket)
{
    uint8_t lengthPrefix[4];
    socket.recvExactly(lengthPrefix, sizeof(lengthPrefix));
    const uint32_t length = util::readU32BE(lengthPrefix);

    if (length == 0) {
        return std::nullopt; // keep-alive
    }
    if (length > kMaxMessageSize) {
        throw ProtocolError("message: frame of " + std::to_string(length) +
                            " bytes exceeds limit");
    }

    // Once the length prefix has been consumed, a timeout is no longer
    // benign: giving up mid-frame would desynchronize the stream.
    try {
        uint8_t id = 0;
        socket.recvExactly(&id, 1);

        Message message;
        message.type = static_cast<MessageType>(id);
        message.payload.resize(length - 1);
        if (!message.payload.empty()) {
            socket.recvExactly(message.payload.data(), message.payload.size());
        }
        return message;
    } catch (const TimeoutError&) {
        throw NetworkError("recv timed out mid-message");
    }
}

void writeMessage(net::TcpSocket& socket, MessageType type,
                  const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame;
    frame.reserve(5 + payload.size());
    util::appendU32BE(frame, static_cast<uint32_t>(payload.size() + 1));
    frame.push_back(static_cast<uint8_t>(type));
    frame.insert(frame.end(), payload.begin(), payload.end());
    socket.sendAll(frame.data(), frame.size());
}

void writeKeepAlive(net::TcpSocket& socket)
{
    const uint8_t frame[4] = { 0, 0, 0, 0 };
    socket.sendAll(frame, sizeof(frame));
}

std::vector<uint8_t> encodeRequest(const BlockRequest& request)
{
    std::vector<uint8_t> payload;
    payload.reserve(12);
    util::appendU32BE(payload, request.pieceIndex);
    util::appendU32BE(payload, request.begin);
    util::appendU32BE(payload, request.length);
    return payload;
}

BlockRequest decodeRequest(const Message& message)
{
    requirePayloadSize(message, 12, "request");
    BlockRequest request;
    request.pieceIndex = util::readU32BE(message.payload.data());
    request.begin = util::readU32BE(message.payload.data() + 4);
    request.length = util::readU32BE(message.payload.data() + 8);
    return request;
}

PieceBlock decodePiece(Message&& message)
{
    if (message.payload.size() < 8) {
        throw ProtocolError("message: piece payload too short");
    }
    PieceBlock block;
    block.pieceIndex = util::readU32BE(message.payload.data());
    block.begin = util::readU32BE(message.payload.data() + 4);
    block.data.assign(message.payload.begin() + 8, message.payload.end());
    return block;
}

uint32_t decodeHave(const Message& message)
{
    requirePayloadSize(message, 4, "have");
    return util::readU32BE(message.payload.data());
}

} // namespace tx::protocol
