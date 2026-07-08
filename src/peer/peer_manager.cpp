#include "torrentx/peer/peer_manager.hpp"

#include "torrentx/peer/peer_connection.hpp"
#include "torrentx/util/error.hpp"
#include "torrentx/util/logger.hpp"

#include <chrono>

namespace tx::peer {

PeerManager::PeerManager(const crypto::Sha1Digest& infoHash,
                         std::string peerId,
                         download::PieceManager& pieces,
                         size_t maxConnections)
    : m_infoHash(infoHash)
    , m_peerId(std::move(peerId))
    , m_pieces(pieces)
    , m_maxConnections(maxConnections)
{
}

PeerManager::~PeerManager()
{
    stop();
}

void PeerManager::addPeers(const std::vector<PeerAddress>& addresses)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t added = 0;
    for (const PeerAddress& address : addresses) {
        if (m_known.insert(address).second) {
            m_queue.push_back(address);
            ++added;
        }
    }
    if (added > 0) {
        m_queueCv.notify_all();
    }
}

void PeerManager::start()
{
    m_stop = false;
    for (size_t i = 0; i < m_maxConnections; ++i) {
        m_workers.emplace_back(&PeerManager::workerLoop, this);
    }
}

void PeerManager::stop()
{
    m_stop = true;
    m_queueCv.notify_all();
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_workers.clear();
}

size_t PeerManager::queuedCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

bool PeerManager::takeNext(PeerAddress& out)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    // Wake periodically so workers notice stop/completion even when no
    // new addresses ever arrive.
    m_queueCv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return m_stop.load() || !m_queue.empty(); });
    if (m_stop || m_queue.empty()) {
        return false;
    }
    out = m_queue.front();
    m_queue.pop_front();
    ++m_attempts[out.endpoint()];
    return true;
}

void PeerManager::workerLoop()
{
    while (!m_stop && !m_pieces.isComplete()) {
        PeerAddress address;
        if (!takeNext(address)) {
            continue; // Timed out waiting; re-check the exit conditions.
        }

        // Decrements the connected counter however the connection ends.
        struct ConnectedGuard {
            std::atomic<size_t>& counter;
            bool active = false;
            void activate() { counter++; active = true; }
            ~ConnectedGuard() { if (active) --counter; }
        } guard{ m_connected };

        try {
            PeerConnection connection(m_nextKey++, address, m_infoHash,
                                      m_peerId, m_pieces, m_stop);
            connection.run([&guard] { guard.activate(); });
        } catch (const Error& e) {
            log::debug("peer ", address.endpoint(), ": ", e.what());

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_attempts[address.endpoint()] < kMaxAttemptsPerPeer) {
                m_queue.push_back(address); // One more chance later.
            }
        }
    }
}

} // namespace tx::peer
