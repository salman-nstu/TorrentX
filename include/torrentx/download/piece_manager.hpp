#pragma once

#include "torrentx/crypto/sha1.hpp"
#include "torrentx/download/file_storage.hpp"
#include "torrentx/protocol/bitfield.hpp"
#include "torrentx/protocol/message.hpp"
#include "torrentx/torrent/torrent_file.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace tx::download {

/// Tracks the state of every piece and block, schedules block requests
/// across peers, verifies completed pieces against their SHA-1 hashes and
/// writes verified data to storage.
///
/// Thread-safe: called concurrently from every peer connection thread.
class PieceManager {
public:
    /// Standard request granularity (BEP 3 recommends 16 KiB blocks).
    static constexpr uint32_t kBlockSize = 16 * 1024;

    /// A block re-request becomes possible when its original request has
    /// been outstanding this long — recovers work from stalled peers.
    static constexpr std::chrono::seconds kRequestTimeout{20};

    PieceManager(const torrent::TorrentFile& torrent, FileStorage& storage);

    /// Hashes data already on disk and marks matching pieces complete.
    /// Enables resume and powers the `verify` command. Returns the number
    /// of complete pieces. `progress(checked, total)` is optional.
    size_t verifyExisting(const std::function<void(size_t, size_t)>& progress = {});

    /// Picks the next block to request from a peer owning `peerPieces`.
    /// Returns std::nullopt when the peer has nothing useful right now.
    /// `peerKey` identifies the connection for later release/steal logic.
    std::optional<protocol::BlockRequest>
    nextRequest(const protocol::Bitfield& peerPieces, int peerKey);

    /// Stores a received block. When it completes a piece, the piece is
    /// hash-checked and either written to disk or reset for re-download.
    void onBlockReceived(int peerKey, const protocol::PieceBlock& block);

    /// Returns blocks requested by this peer to the pool (disconnect/choke).
    void releaseBlocks(int peerKey);

    /// True when the peer advertises at least one piece we still need.
    bool isInteresting(const protocol::Bitfield& peerPieces) const;

    bool isComplete() const;
    size_t completedPieces() const;
    size_t totalPieces() const { return m_pieces.size(); }
    int64_t totalBytes() const { return m_totalBytes; }

    /// Bytes belonging to hash-verified pieces (drives `left`/progress).
    int64_t verifiedBytes() const;

    /// Raw payload bytes received from peers (drives the rate display;
    /// includes data later discarded by failed hash checks).
    int64_t rawDownloadedBytes() const;

    size_t hashFailures() const;

private:
    enum class BlockState : uint8_t { Missing, Requested, Received };
    enum class PieceState : uint8_t { Missing, InProgress, Complete };

    struct Block {
        BlockState state = BlockState::Missing;
        int owner = -1;
        std::chrono::steady_clock::time_point requestedAt{};
    };

    struct Piece {
        PieceState state = PieceState::Missing;
        std::vector<Block> blocks;
        std::vector<uint8_t> buffer; ///< Allocated only while InProgress.
        size_t receivedBlocks = 0;
    };

    uint32_t blockLength(size_t piece, size_t blockIndex) const;
    void startPiece(size_t index);
    void resetPiece(size_t index);
    void finishPiece(size_t index);

    const torrent::TorrentFile& m_torrent;
    FileStorage& m_storage;

    mutable std::mutex m_mutex;
    std::vector<Piece> m_pieces;
    int64_t m_totalBytes = 0;
    int64_t m_verifiedBytes = 0;
    int64_t m_rawDownloaded = 0;
    size_t m_completed = 0;
    size_t m_hashFailures = 0;
};

} // namespace tx::download
