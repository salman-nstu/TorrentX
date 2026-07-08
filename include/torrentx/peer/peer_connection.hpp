#pragma once

#include "torrentx/crypto/sha1.hpp"
#include "torrentx/download/piece_manager.hpp"
#include "torrentx/peer/peer_address.hpp"
#include "torrentx/protocol/bitfield.hpp"
#include "torrentx/util/tcp_socket.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace tx::peer {

/// One outgoing connection to a remote peer: TCP connect, BitTorrent
/// handshake with info-hash verification, bitfield exchange, then the
/// choke/interested state machine with a pipelined request loop.
///
/// Runs synchronously on the calling (worker) thread until the download
/// completes, the stop flag is raised, or the connection fails (throws).
class PeerConnection {
public:
    static constexpr int kConnectTimeoutMs = 10'000;

    /// Short enough that a stalled peer is detected quickly and that
    /// stop/completion is noticed promptly by threads blocked in recv;
    /// idle-but-alive peers get several rounds before being dropped.
    static constexpr int kReceiveTimeoutMs = 15'000;

    static constexpr int kPipelineDepth = 16;

    PeerConnection(int key,
                   PeerAddress address,
                   const crypto::Sha1Digest& infoHash,
                   const std::string& peerId,
                   download::PieceManager& pieces,
                   const std::atomic<bool>& stop);

    ~PeerConnection();

    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    /// Connects and runs the message loop. `onEstablished` fires once the
    /// handshake has been verified. Throws tx::NetworkError /
    /// tx::ProtocolError when the peer is unusable.
    void run(const std::function<void()>& onEstablished = {});

    /// Peer id the remote sent in its handshake (empty before handshake).
    const std::string& remotePeerId() const { return m_remotePeerId; }

private:
    void establish();       ///< Connect + handshake + verification.
    void messageLoop();
    void fillPipeline();
    bool handleMessage();   ///< Returns false when the peer became useless.
    void updateInterest();

    const int m_key;
    const PeerAddress m_address;
    const crypto::Sha1Digest& m_infoHash;
    const std::string& m_peerId;
    download::PieceManager& m_pieces;
    const std::atomic<bool>& m_stop;

    net::TcpSocket m_socket;
    protocol::Bitfield m_remotePieces;
    std::string m_remotePeerId;

    bool m_peerChoking = true;
    bool m_amInterested = false;
    int m_outstanding = 0;      ///< Requests sent but not yet answered.
    int m_idleRounds = 0;       ///< Consecutive receive timeouts.
    std::chrono::steady_clock::time_point m_lastSend{};
};

} // namespace tx::peer
