#pragma once

#include "torrentx/torrent/torrent_file.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tx::download {

/// User-tunable download settings.
struct DownloadOptions {
    std::filesystem::path outputDir = ".";
    size_t maxPeers = 30;      ///< Worker threads / simultaneous connections.
    uint16_t listenPort = 6881; ///< Advertised to the tracker (outgoing-only client).
};

/// Result of a completed (or aborted) download run.
enum class DownloadResult {
    Completed,
    Aborted,   ///< Stopped by the caller (e.g. Ctrl+C).
};

/// Ties every module together: opens storage, resumes from existing data,
/// announces to the tracker, feeds peers to the PeerManager, re-announces
/// periodically, and renders console progress until the torrent is done.
class DownloadEngine {
public:
    DownloadEngine(const torrent::TorrentFile& torrent, DownloadOptions options);

    /// Runs to completion. `stopRequested` may be flipped from a signal
    /// handler to abort gracefully (a `stopped` announce is still sent).
    DownloadResult run(const std::atomic<bool>& stopRequested);

private:
    const torrent::TorrentFile& m_torrent;
    DownloadOptions m_options;
    std::string m_peerId;
};

} // namespace tx::download
