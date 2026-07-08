#pragma once

#include "torrentx/crypto/sha1.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tx::torrent {

/// One payload file inside a torrent, with its position in the
/// concatenated byte stream that pieces are hashed over.
struct FileEntry {
    std::string path;     ///< Relative path ("dir/sub/name.ext" for multi-file).
    int64_t length = 0;   ///< Size in bytes.
    int64_t offset = 0;   ///< Offset of the first byte in the global stream.
};

/// Immutable, validated representation of a .torrent metainfo file
/// (BEP 3, plus the announce-list extension from BEP 12).
class TorrentFile {
public:
    /// Reads and parses a .torrent file from disk.
    static TorrentFile loadFromFile(const std::string& path);

    /// Parses .torrent data already in memory.
    static TorrentFile loadFromBuffer(std::string_view buffer);

    const std::string& name() const { return m_name; }
    const std::string& announce() const { return m_announce; }

    /// Tracker tiers from announce-list; falls back to a single tier
    /// containing `announce` when the extension is absent.
    const std::vector<std::vector<std::string>>& announceTiers() const { return m_announceTiers; }

    const crypto::Sha1Digest& infoHash() const { return m_infoHash; }
    std::string infoHashHex() const;

    int64_t pieceLength() const { return m_pieceLength; }
    const std::vector<crypto::Sha1Digest>& pieceHashes() const { return m_pieceHashes; }
    size_t pieceCount() const { return m_pieceHashes.size(); }

    /// Actual length of one piece; the final piece is usually shorter.
    int64_t pieceSize(size_t index) const;

    const std::vector<FileEntry>& files() const { return m_files; }
    bool isMultiFile() const { return m_multiFile; }
    int64_t totalLength() const { return m_totalLength; }

    const std::string& comment() const { return m_comment; }
    const std::string& createdBy() const { return m_createdBy; }
    int64_t creationDate() const { return m_creationDate; }

private:
    TorrentFile() = default;

    std::string m_name;
    std::string m_announce;
    std::vector<std::vector<std::string>> m_announceTiers;
    crypto::Sha1Digest m_infoHash{};
    int64_t m_pieceLength = 0;
    std::vector<crypto::Sha1Digest> m_pieceHashes;
    std::vector<FileEntry> m_files;
    bool m_multiFile = false;
    int64_t m_totalLength = 0;
    std::string m_comment;
    std::string m_createdBy;
    int64_t m_creationDate = 0;
};

} // namespace tx::torrent
