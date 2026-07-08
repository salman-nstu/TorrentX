#pragma once

#include "torrentx/crypto/sha1.hpp"
#include "torrentx/download/piece_manager.hpp"
#include "torrentx/peer/peer_address.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace tx::peer {

/// Owns the pool of peer worker threads. Each worker repeatedly takes an
/// address from the queue, runs a PeerConnection on it, and reports the
/// outcome; failed addresses get a bounded number of retries.
class PeerManager {
public:
    static constexpr int kMaxAttemptsPerPeer = 2;

    PeerManager(const crypto::Sha1Digest& infoHash,
                std::string peerId,
                download::PieceManager& pieces,
                size_t maxConnections);

    ~PeerManager();

    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    /// Queues tracker-provided addresses, ignoring ones already known.
    void addPeers(const std::vector<PeerAddress>& addresses);

    void start();

    /// Signals all workers and joins them. Safe to call more than once.
    void stop();

    /// Peers that completed the handshake and are currently connected.
    size_t connectedCount() const { return m_connected; }

    /// Addresses waiting to be tried.
    size_t queuedCount() const;

private:
    void workerLoop();
    bool takeNext(PeerAddress& out);

    const crypto::Sha1Digest m_infoHash;
    const std::string m_peerId;
    download::PieceManager& m_pieces;
    const size_t m_maxConnections;

    mutable std::mutex m_mutex;
    std::condition_variable m_queueCv;
    std::deque<PeerAddress> m_queue;
    std::set<PeerAddress> m_known;
    std::map<std::string, int> m_attempts; ///< endpoint -> tries so far

    std::vector<std::thread> m_workers;
    std::atomic<bool> m_stop{false};
    std::atomic<size_t> m_connected{0};
    std::atomic<int> m_nextKey{0};
};

} // namespace tx::peer
