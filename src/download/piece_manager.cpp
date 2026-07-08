#include "torrentx/download/piece_manager.hpp"

#include "torrentx/util/error.hpp"
#include "torrentx/util/logger.hpp"

namespace tx::download {

using Clock = std::chrono::steady_clock;

PieceManager::PieceManager(const torrent::TorrentFile& torrent, FileStorage& storage)
    : m_torrent(torrent)
    , m_storage(storage)
    , m_totalBytes(torrent.totalLength())
{
    m_pieces.resize(torrent.pieceCount());
    for (size_t i = 0; i < m_pieces.size(); ++i) {
        const auto size = static_cast<uint64_t>(torrent.pieceSize(i));
        const size_t blockCount = static_cast<size_t>((size + kBlockSize - 1) / kBlockSize);
        m_pieces[i].blocks.resize(blockCount);
    }
}

uint32_t PieceManager::blockLength(size_t piece, size_t blockIndex) const
{
    const int64_t pieceSize = m_torrent.pieceSize(piece);
    const int64_t begin = static_cast<int64_t>(blockIndex) * kBlockSize;
    return static_cast<uint32_t>(std::min<int64_t>(kBlockSize, pieceSize - begin));
}

size_t PieceManager::verifyExisting(const std::function<void(size_t, size_t)>& progress)
{
    std::vector<uint8_t> buffer;
    for (size_t i = 0; i < m_pieces.size(); ++i) {
        const int64_t size = m_torrent.pieceSize(i);
        buffer.resize(static_cast<size_t>(size));

        const int64_t offset = static_cast<int64_t>(i) * m_torrent.pieceLength();
        // Reading and hashing happen outside the lock: verification runs
        // before peer threads start (resume) or without any (verify).
        if (m_storage.read(offset, buffer.data(), size)) {
            const crypto::Sha1Digest digest = crypto::sha1(buffer.data(), buffer.size());
            if (digest == m_torrent.pieceHashes()[i]) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_pieces[i].state != PieceState::Complete) {
                    m_pieces[i].state = PieceState::Complete;
                    m_verifiedBytes += size;
                    ++m_completed;
                }
            }
        }
        if (progress) {
            progress(i + 1, m_pieces.size());
        }
    }
    return completedPieces();
}

void PieceManager::startPiece(size_t index)
{
    Piece& piece = m_pieces[index];
    piece.state = PieceState::InProgress;
    piece.buffer.assign(static_cast<size_t>(m_torrent.pieceSize(index)), 0);
    piece.receivedBlocks = 0;
    for (Block& block : piece.blocks) {
        block = Block{};
    }
}

void PieceManager::resetPiece(size_t index)
{
    Piece& piece = m_pieces[index];
    piece.state = PieceState::Missing;
    piece.buffer.clear();
    piece.buffer.shrink_to_fit();
    piece.receivedBlocks = 0;
    for (Block& block : piece.blocks) {
        block = Block{};
    }
}

std::optional<protocol::BlockRequest>
PieceManager::nextRequest(const protocol::Bitfield& peerPieces, int peerKey)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto now = Clock::now();

    // Prefer finishing pieces that are already in flight; fall back to a
    // block whose request has been outstanding too long (stalled peer).
    std::optional<protocol::BlockRequest> stalled;
    for (size_t i = 0; i < m_pieces.size(); ++i) {
        Piece& piece = m_pieces[i];
        if (piece.state != PieceState::InProgress || !peerPieces.has(i)) {
            continue;
        }
        for (size_t b = 0; b < piece.blocks.size(); ++b) {
            Block& block = piece.blocks[b];
            if (block.state == BlockState::Missing) {
                block.state = BlockState::Requested;
                block.owner = peerKey;
                block.requestedAt = now;
                return protocol::BlockRequest{ static_cast<uint32_t>(i),
                                               static_cast<uint32_t>(b) * kBlockSize,
                                               blockLength(i, b) };
            }
            if (!stalled && block.state == BlockState::Requested &&
                block.owner != peerKey && now - block.requestedAt > kRequestTimeout) {
                stalled = protocol::BlockRequest{ static_cast<uint32_t>(i),
                                                  static_cast<uint32_t>(b) * kBlockSize,
                                                  blockLength(i, b) };
            }
        }
    }

    // Nothing in flight suits this peer: open a fresh piece it has.
    for (size_t i = 0; i < m_pieces.size(); ++i) {
        if (m_pieces[i].state == PieceState::Missing && peerPieces.has(i)) {
            startPiece(i);
            Block& block = m_pieces[i].blocks[0];
            block.state = BlockState::Requested;
            block.owner = peerKey;
            block.requestedAt = now;
            return protocol::BlockRequest{ static_cast<uint32_t>(i), 0, blockLength(i, 0) };
        }
    }

    if (stalled) {
        Piece& piece = m_pieces[stalled->pieceIndex];
        Block& block = piece.blocks[stalled->begin / kBlockSize];
        block.owner = peerKey;
        block.requestedAt = now;
        return stalled;
    }
    return std::nullopt;
}

void PieceManager::onBlockReceived(int peerKey, const protocol::PieceBlock& received)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (received.pieceIndex >= m_pieces.size()) {
        throw ProtocolError("piece message with invalid index");
    }
    Piece& piece = m_pieces[received.pieceIndex];
    m_rawDownloaded += static_cast<int64_t>(received.data.size());

    if (piece.state != PieceState::InProgress) {
        return; // Late duplicate for a piece that completed or was reset.
    }
    if (received.begin % kBlockSize != 0) {
        throw ProtocolError("piece message with misaligned offset");
    }
    const size_t blockIndex = received.begin / kBlockSize;
    if (blockIndex >= piece.blocks.size() ||
        received.data.size() != blockLength(received.pieceIndex, blockIndex)) {
        throw ProtocolError("piece message with invalid offset/length");
    }

    Block& block = piece.blocks[blockIndex];
    if (block.state == BlockState::Received) {
        return; // Duplicate delivery after a timeout steal.
    }

    std::copy(received.data.begin(), received.data.end(),
              piece.buffer.begin() + received.begin);
    block.state = BlockState::Received;
    block.owner = peerKey;
    ++piece.receivedBlocks;

    if (piece.receivedBlocks == piece.blocks.size()) {
        finishPiece(received.pieceIndex);
    }
}

void PieceManager::finishPiece(size_t index)
{
    // Called with m_mutex held.
    Piece& piece = m_pieces[index];
    const crypto::Sha1Digest digest = crypto::sha1(piece.buffer.data(), piece.buffer.size());

    if (digest != m_torrent.pieceHashes()[index]) {
        ++m_hashFailures;
        log::warn("piece ", index, " failed hash check, re-downloading");
        resetPiece(index);
        return;
    }

    const int64_t offset = static_cast<int64_t>(index) * m_torrent.pieceLength();
    m_storage.write(offset, piece.buffer.data(), static_cast<int64_t>(piece.buffer.size()));

    m_verifiedBytes += static_cast<int64_t>(piece.buffer.size());
    ++m_completed;
    piece.state = PieceState::Complete;
    piece.buffer.clear();
    piece.buffer.shrink_to_fit();
    log::debug("piece ", index, " verified and written (",
               m_completed, "/", m_pieces.size(), ")");
}

void PieceManager::releaseBlocks(int peerKey)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Piece& piece : m_pieces) {
        if (piece.state != PieceState::InProgress) {
            continue;
        }
        for (Block& block : piece.blocks) {
            if (block.state == BlockState::Requested && block.owner == peerKey) {
                block.state = BlockState::Missing;
                block.owner = -1;
            }
        }
    }
}

bool PieceManager::isInteresting(const protocol::Bitfield& peerPieces) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_pieces.size(); ++i) {
        if (m_pieces[i].state != PieceState::Complete && peerPieces.has(i)) {
            return true;
        }
    }
    return false;
}

bool PieceManager::isComplete() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_completed == m_pieces.size();
}

size_t PieceManager::completedPieces() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_completed;
}

int64_t PieceManager::verifiedBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_verifiedBytes;
}

int64_t PieceManager::rawDownloadedBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rawDownloaded;
}

size_t PieceManager::hashFailures() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hashFailures;
}

} // namespace tx::download
