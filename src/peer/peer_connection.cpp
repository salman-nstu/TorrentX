#include "torrentx/peer/peer_connection.hpp"

#include "torrentx/protocol/handshake.hpp"
#include "torrentx/protocol/message.hpp"
#include "torrentx/util/byte_utils.hpp"
#include "torrentx/util/error.hpp"
#include "torrentx/util/logger.hpp"

namespace tx::peer {

using Clock = std::chrono::steady_clock;

namespace {

/// BEP 3 tells peers to send a keep-alive if nothing else was sent for a
/// while; peers commonly drop connections silent for over two minutes.
constexpr std::chrono::seconds kKeepAliveInterval{90};

/// Give up on a peer that produced no message at all for this many
/// consecutive receive timeouts.
constexpr int kMaxIdleRounds = 3;

} // namespace

PeerConnection::PeerConnection(int key,
                               PeerAddress address,
                               const crypto::Sha1Digest& infoHash,
                               const std::string& peerId,
                               download::PieceManager& pieces,
                               const std::atomic<bool>& stop)
    : m_key(key)
    , m_address(std::move(address))
    , m_infoHash(infoHash)
    , m_peerId(peerId)
    , m_pieces(pieces)
    , m_stop(stop)
    , m_remotePieces(pieces.totalPieces())
{
}

PeerConnection::~PeerConnection()
{
    // Whatever happens to this connection, its claimed blocks must go
    // back to the pool or the download would stall waiting on a ghost.
    m_pieces.releaseBlocks(m_key);
}

void PeerConnection::run(const std::function<void()>& onEstablished)
{
    establish();
    if (onEstablished) {
        onEstablished();
    }
    messageLoop();
}

void PeerConnection::establish()
{
    m_socket.connect(m_address.ip, m_address.port, kConnectTimeoutMs);
    m_socket.setReceiveTimeout(kConnectTimeoutMs);

    const protocol::Handshake ours = protocol::Handshake::make(m_infoHash, m_peerId);
    const auto raw = ours.serialize();
    m_socket.sendAll(raw.data(), raw.size());

    std::array<uint8_t, protocol::Handshake::kSize> received{};
    m_socket.recvExactly(received.data(), received.size());
    const protocol::Handshake theirs = protocol::Handshake::parse(received);

    // A peer serving a different torrent is useless and possibly hostile.
    if (theirs.infoHash != m_infoHash) {
        throw ProtocolError("handshake: info hash mismatch");
    }
    m_remotePeerId.assign(theirs.peerId.begin(), theirs.peerId.end());

    m_socket.setReceiveTimeout(kReceiveTimeoutMs);
    m_lastSend = Clock::now();
    log::debug("handshake ok with ", m_address.endpoint());
}

void PeerConnection::updateInterest()
{
    const bool interesting = m_pieces.isInteresting(m_remotePieces);
    if (interesting && !m_amInterested) {
        protocol::writeMessage(m_socket, protocol::MessageType::Interested);
        m_amInterested = true;
        m_lastSend = Clock::now();
    } else if (!interesting && m_amInterested) {
        protocol::writeMessage(m_socket, protocol::MessageType::NotInterested);
        m_amInterested = false;
        m_lastSend = Clock::now();
    }
}

void PeerConnection::fillPipeline()
{
    while (!m_peerChoking && m_outstanding < kPipelineDepth) {
        const auto request = m_pieces.nextRequest(m_remotePieces, m_key);
        if (!request) {
            break;
        }
        protocol::writeMessage(m_socket, protocol::MessageType::Request,
                               protocol::encodeRequest(*request));
        ++m_outstanding;
        m_lastSend = Clock::now();
    }
}

void PeerConnection::messageLoop()
{
    while (!m_stop && !m_pieces.isComplete()) {
        updateInterest();
        fillPipeline();

        if (Clock::now() - m_lastSend > kKeepAliveInterval) {
            protocol::writeKeepAlive(m_socket);
            m_lastSend = Clock::now();
        }

        if (!handleMessage()) {
            return;
        }
    }
}

bool PeerConnection::handleMessage()
{
    std::optional<protocol::Message> message;
    try {
        message = protocol::readMessage(m_socket);
        m_idleRounds = 0;
    } catch (const TimeoutError&) {
        // No traffic inside the receive window. Tolerate a few idle
        // rounds (we might simply be choked), then move on to another
        // peer — with requests outstanding this peer is stalling us.
        ++m_idleRounds;
        if (m_outstanding > 0 || m_idleRounds >= kMaxIdleRounds) {
            throw NetworkError("peer went silent");
        }
        return true;
    }

    if (!message) {
        return true; // Keep-alive.
    }

    switch (message->type) {
    case protocol::MessageType::Choke:
        m_peerChoking = true;
        // The remote discards our queued requests when it chokes us.
        m_pieces.releaseBlocks(m_key);
        m_outstanding = 0;
        break;

    case protocol::MessageType::Unchoke:
        m_peerChoking = false;
        break;

    case protocol::MessageType::Have:
        m_remotePieces.set(protocol::decodeHave(*message));
        break;

    case protocol::MessageType::Bitfield:
        if (!m_remotePieces.assign(message->payload, m_pieces.totalPieces())) {
            throw ProtocolError("invalid bitfield from " + m_address.endpoint());
        }
        break;

    case protocol::MessageType::Piece: {
        protocol::PieceBlock block = protocol::decodePiece(std::move(*message));
        m_pieces.onBlockReceived(m_key, block);
        if (m_outstanding > 0) {
            --m_outstanding;
        }
        break;
    }

    case protocol::MessageType::Interested:
    case protocol::MessageType::NotInterested:
    case protocol::MessageType::Request:
    case protocol::MessageType::Cancel:
    case protocol::MessageType::Port:
        // TorrentX is download-only: we never unchoke anyone, so upload
        // related messages need no reaction.
        break;
    }
    return true;
}

} // namespace tx::peer
