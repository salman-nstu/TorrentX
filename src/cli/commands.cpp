#include "commands.hpp"

#include "torrentx/download/download_engine.hpp"
#include "torrentx/download/file_storage.hpp"
#include "torrentx/download/piece_manager.hpp"
#include "torrentx/torrent/torrent_file.hpp"
#include "torrentx/tracker/tracker_client.hpp"
#include "torrentx/util/byte_utils.hpp"
#include "torrentx/util/logger.hpp"

#include <cinttypes>
#include <cstdio>
#include <ctime>

namespace tx::cli {

namespace {

std::string formatTimestamp(int64_t unixSeconds)
{
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buffer;
}

/// Announce with sensible defaults for the metadata-only commands.
tracker::AnnounceResponse announceOnce(const torrent::TorrentFile& torrent,
                                       const Options& options)
{
    tracker::TrackerClient client(torrent);
    tracker::AnnounceRequest request;
    request.infoHash = torrent.infoHash();
    request.peerId = util::generatePeerId();
    request.port = options.port;
    request.left = torrent.totalLength();
    request.event = tracker::Event::Started;
    return client.announce(request);
}

} // namespace

int cmdInfo(const Options& options)
{
    const auto torrent = torrent::TorrentFile::loadFromFile(options.torrentPath);

    std::printf("Name:          %s\n", torrent.name().c_str());
    std::printf("Info hash:     %s\n", torrent.infoHashHex().c_str());
    std::printf("Total size:    %s (%" PRId64 " bytes)\n",
                util::formatSize(static_cast<uint64_t>(torrent.totalLength())).c_str(),
                torrent.totalLength());
    std::printf("Piece length:  %s\n",
                util::formatSize(static_cast<uint64_t>(torrent.pieceLength())).c_str());
    std::printf("Pieces:        %zu\n", torrent.pieceCount());
    std::printf("Announce:      %s\n", torrent.announce().c_str());

    if (torrent.announceTiers().size() > 1 || torrent.announceTiers()[0].size() > 1) {
        std::printf("Tracker tiers:\n");
        for (size_t tier = 0; tier < torrent.announceTiers().size(); ++tier) {
            for (const std::string& url : torrent.announceTiers()[tier]) {
                std::printf("  [%zu] %s\n", tier, url.c_str());
            }
        }
    }
    if (!torrent.comment().empty()) {
        std::printf("Comment:       %s\n", torrent.comment().c_str());
    }
    if (!torrent.createdBy().empty()) {
        std::printf("Created by:    %s\n", torrent.createdBy().c_str());
    }
    if (torrent.creationDate() > 0) {
        std::printf("Created:       %s\n", formatTimestamp(torrent.creationDate()).c_str());
    }

    std::printf("Mode:          %s\n", torrent.isMultiFile() ? "multi-file" : "single-file");
    std::printf("Files:         %zu\n", torrent.files().size());
    for (const torrent::FileEntry& file : torrent.files()) {
        std::printf("  %10s  %s\n",
                    util::formatSize(static_cast<uint64_t>(file.length)).c_str(),
                    file.path.c_str());
    }
    return 0;
}

int cmdPeers(const Options& options)
{
    const auto torrent = torrent::TorrentFile::loadFromFile(options.torrentPath);
    const auto response = announceOnce(torrent, options);

    std::printf("Tracker:   %s\n", response.trackerUrl.c_str());
    std::printf("Interval:  %d s\n", response.interval);
    if (response.seeders >= 0) {
        std::printf("Seeders:   %" PRId64 "\n", response.seeders);
    }
    if (response.leechers >= 0) {
        std::printf("Leechers:  %" PRId64 "\n", response.leechers);
    }
    std::printf("Peers (%zu):\n", response.peers.size());
    for (const peer::PeerAddress& address : response.peers) {
        std::printf("  %s\n", address.endpoint().c_str());
    }
    return 0;
}

int cmdDownload(const Options& options, const std::atomic<bool>& stopRequested)
{
    const auto torrent = torrent::TorrentFile::loadFromFile(options.torrentPath);
    log::info("downloading '", torrent.name(), "' (",
              util::formatSize(static_cast<uint64_t>(torrent.totalLength())),
              ", ", torrent.pieceCount(), " pieces) to ", options.outputDir);

    download::DownloadOptions engineOptions;
    engineOptions.outputDir = options.outputDir;
    engineOptions.maxPeers = options.maxPeers;
    engineOptions.listenPort = options.port;

    download::DownloadEngine engine(torrent, engineOptions);
    const auto result = engine.run(stopRequested);
    return result == download::DownloadResult::Completed ? 0 : 130;
}

int cmdVerify(const Options& options)
{
    const auto torrent = torrent::TorrentFile::loadFromFile(options.torrentPath);

    download::FileStorage storage(torrent, options.outputDir);
    storage.open(download::FileStorage::Mode::ReadOnly);

    download::PieceManager pieces(torrent, storage);
    pieces.verifyExisting([](size_t checked, size_t total) {
        if (checked % 64 == 0 || checked == total) {
            std::printf("\rverifying %zu/%zu pieces", checked, total);
            std::fflush(stdout);
        }
    });
    std::printf("\n");

    const size_t complete = pieces.completedPieces();
    const double percent = pieces.totalPieces() > 0
        ? 100.0 * static_cast<double>(complete) / static_cast<double>(pieces.totalPieces())
        : 100.0;

    std::printf("%zu/%zu pieces valid (%.1f%%), %s of %s\n",
                complete, pieces.totalPieces(), percent,
                util::formatSize(static_cast<uint64_t>(pieces.verifiedBytes())).c_str(),
                util::formatSize(static_cast<uint64_t>(pieces.totalBytes())).c_str());

    return complete == pieces.totalPieces() ? 0 : 1;
}

} // namespace tx::cli
