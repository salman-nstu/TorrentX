#include "torrentx/download/download_engine.hpp"

#include "torrentx/download/file_storage.hpp"
#include "torrentx/download/piece_manager.hpp"
#include "torrentx/peer/peer_manager.hpp"
#include "torrentx/tracker/tracker_client.hpp"
#include "torrentx/util/byte_utils.hpp"
#include "torrentx/util/error.hpp"
#include "torrentx/util/logger.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

namespace tx::download {

using Clock = std::chrono::steady_clock;

namespace {

/// Renders a single-line progress bar to stdout, overwriting in place.
void renderProgress(const PieceManager& pieces, size_t connectedPeers,
                    double bytesPerSecond)
{
    const double fraction = pieces.totalBytes() > 0
        ? static_cast<double>(pieces.verifiedBytes()) / static_cast<double>(pieces.totalBytes())
        : 1.0;

    constexpr int kBarWidth = 24;
    const int filled = static_cast<int>(fraction * kBarWidth);
    std::string bar(static_cast<size_t>(filled), '=');
    if (filled < kBarWidth) {
        bar += '>';
        bar.append(static_cast<size_t>(kBarWidth - filled - 1), ' ');
    }

    const int64_t remaining = pieces.totalBytes() - pieces.verifiedBytes();
    const int64_t etaSeconds = bytesPerSecond > 1.0
        ? static_cast<int64_t>(static_cast<double>(remaining) / bytesPerSecond)
        : -1;

    std::printf("\r[%s] %5.1f%%  %zu/%zu pieces  %s/s  peers %zu  ETA %s   ",
                bar.c_str(), fraction * 100.0,
                pieces.completedPieces(), pieces.totalPieces(),
                util::formatSize(static_cast<uint64_t>(bytesPerSecond)).c_str(),
                connectedPeers,
                util::formatEta(etaSeconds).c_str());
    std::fflush(stdout);
}

} // namespace

DownloadEngine::DownloadEngine(const torrent::TorrentFile& torrent, DownloadOptions options)
    : m_torrent(torrent)
    , m_options(std::move(options))
    , m_peerId(util::generatePeerId())
{
}

DownloadResult DownloadEngine::run(const std::atomic<bool>& stopRequested)
{
    FileStorage storage(m_torrent, m_options.outputDir);
    storage.open(FileStorage::Mode::ReadWrite);

    PieceManager pieces(m_torrent, storage);

    // Resume: hash whatever is already on disk before asking the swarm.
    if (storage.hasPreexistingData()) {
        log::info("checking existing data...");
        pieces.verifyExisting([](size_t checked, size_t total) {
            if (checked % 64 == 0 || checked == total) {
                std::printf("\rverifying %zu/%zu pieces", checked, total);
                std::fflush(stdout);
            }
        });
        std::printf("\n");
        log::info("resume: ", pieces.completedPieces(), "/", pieces.totalPieces(),
                  " pieces already complete");
    }

    if (pieces.isComplete()) {
        log::info("torrent already complete, nothing to download");
        return DownloadResult::Completed;
    }

    tracker::TrackerClient trackerClient(m_torrent);
    tracker::AnnounceRequest announce;
    announce.infoHash = m_torrent.infoHash();
    announce.peerId = m_peerId;
    announce.port = m_options.listenPort;
    announce.left = pieces.totalBytes() - pieces.verifiedBytes();
    announce.event = tracker::Event::Started;

    tracker::AnnounceResponse response = trackerClient.announce(announce);
    log::info("tracker returned ", response.peers.size(), " peers",
              response.seeders >= 0
                  ? " (seeders: " + std::to_string(response.seeders) +
                    ", leechers: " + std::to_string(response.leechers) + ")"
                  : "");

    peer::PeerManager peers(m_torrent.infoHash(), m_peerId, pieces, m_options.maxPeers);
    peers.addPeers(response.peers);
    peers.start();

    auto lastAnnounce = Clock::now();
    auto announceInterval = std::chrono::seconds(response.interval);

    auto lastTick = Clock::now();
    int64_t lastRaw = pieces.rawDownloadedBytes();
    double smoothedRate = 0.0;

    while (!pieces.isComplete() && !stopRequested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Exponentially smoothed download rate for a stable display.
        const auto now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - lastTick).count();
        const int64_t raw = pieces.rawDownloadedBytes();
        if (seconds > 0) {
            const double instant = static_cast<double>(raw - lastRaw) / seconds;
            smoothedRate = smoothedRate * 0.7 + instant * 0.3;
        }
        lastTick = now;
        lastRaw = raw;

        renderProgress(pieces, peers.connectedCount(), smoothedRate);

        // Re-announce when the tracker interval expires, or early when we
        // are starving for peers.
        const bool starving = peers.connectedCount() == 0 && peers.queuedCount() == 0;
        const auto sinceAnnounce = now - lastAnnounce;
        if (sinceAnnounce > announceInterval ||
            (starving && sinceAnnounce > std::chrono::seconds(30))) {
            try {
                announce.event = tracker::Event::None;
                announce.downloaded = pieces.verifiedBytes();
                announce.left = pieces.totalBytes() - pieces.verifiedBytes();
                response = trackerClient.announce(announce);
                announceInterval = std::chrono::seconds(response.interval);
                peers.addPeers(response.peers);
                log::debug("re-announce: ", response.peers.size(), " peers");
            } catch (const Error& e) {
                log::warn("re-announce failed: ", e.what());
            }
            lastAnnounce = Clock::now();
        }
    }

    std::printf("\n");
    peers.stop();

    // Tell the tracker how this session ended; failures don't matter now.
    try {
        announce.downloaded = pieces.verifiedBytes();
        announce.left = pieces.totalBytes() - pieces.verifiedBytes();
        announce.event = pieces.isComplete() ? tracker::Event::Completed
                                             : tracker::Event::Stopped;
        trackerClient.announce(announce);
    } catch (const Error& e) {
        log::debug("final announce failed: ", e.what());
    }

    if (pieces.hashFailures() > 0) {
        log::warn(pieces.hashFailures(), " pieces failed verification and were re-downloaded");
    }

    if (pieces.isComplete()) {
        log::info("download complete: ",
                  util::formatSize(static_cast<uint64_t>(pieces.totalBytes())),
                  " verified across ", pieces.totalPieces(), " pieces");
        return DownloadResult::Completed;
    }
    log::info("download stopped at ", pieces.completedPieces(), "/",
              pieces.totalPieces(), " pieces; run again to resume");
    return DownloadResult::Aborted;
}

} // namespace tx::download
