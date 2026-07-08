#include "torrentx/torrent/torrent_file.hpp"

#include "torrentx/torrent/bencode.hpp"
#include "torrentx/util/byte_utils.hpp"
#include "torrentx/util/error.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace tx::torrent {

namespace {

/// Validates one path component from the metainfo. Rejecting "..", empty
/// segments and separators prevents a malicious torrent from writing
/// outside the download directory.
void validatePathComponent(const std::string& component)
{
    if (component.empty() || component == "." || component == "..") {
        throw TorrentError("torrent: illegal path component '" + component + "'");
    }
    if (component.find('/') != std::string::npos ||
        component.find('\\') != std::string::npos) {
        throw TorrentError("torrent: path component contains a separator");
    }
}

std::vector<crypto::Sha1Digest> splitPieceHashes(const std::string& pieces)
{
    if (pieces.size() % 20 != 0) {
        throw TorrentError("torrent: 'pieces' length is not a multiple of 20");
    }
    std::vector<crypto::Sha1Digest> hashes(pieces.size() / 20);
    for (size_t i = 0; i < hashes.size(); ++i) {
        std::copy_n(reinterpret_cast<const uint8_t*>(pieces.data()) + i * 20,
                    20, hashes[i].begin());
    }
    return hashes;
}

int64_t requirePositive(int64_t value, const char* what)
{
    if (value <= 0) {
        throw TorrentError(std::string("torrent: ") + what + " must be positive");
    }
    return value;
}

} // namespace

TorrentFile TorrentFile::loadFromFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw TorrentError("cannot open torrent file: " + path);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return loadFromBuffer(buffer.str());
}

TorrentFile TorrentFile::loadFromBuffer(std::string_view buffer)
{
    const bencode::Value root = bencode::parse(buffer);
    if (!root.isDict()) {
        throw TorrentError("torrent: top-level value is not a dictionary");
    }

    TorrentFile t;

    // The info hash identifies the torrent everywhere (tracker, handshake).
    // It must be SHA-1 of the *original* bytes of the info value, which the
    // parser tracked for us, never of a re-encoded copy.
    const bencode::Value& info = root.at("info");
    t.m_infoHash = crypto::sha1(
        reinterpret_cast<const uint8_t*>(buffer.data()) + info.rawBegin(),
        info.rawEnd() - info.rawBegin());

    if (const bencode::Value* announce = root.find("announce")) {
        t.m_announce = announce->asString();
    }

    // BEP 12: announce-list is a list of tiers, each a list of URLs.
    if (const bencode::Value* tiers = root.find("announce-list")) {
        for (const bencode::Value& tier : tiers->asList()) {
            std::vector<std::string> urls;
            for (const bencode::Value& url : tier.asList()) {
                urls.push_back(url.asString());
            }
            if (!urls.empty()) {
                t.m_announceTiers.push_back(std::move(urls));
            }
        }
    }
    if (t.m_announceTiers.empty()) {
        if (t.m_announce.empty()) {
            throw TorrentError("torrent: no announce URL (tracker) present");
        }
        t.m_announceTiers.push_back({ t.m_announce });
    }
    if (t.m_announce.empty()) {
        t.m_announce = t.m_announceTiers.front().front();
    }

    if (const bencode::Value* comment = root.find("comment")) {
        if (comment->isString()) t.m_comment = comment->asString();
    }
    if (const bencode::Value* createdBy = root.find("created by")) {
        if (createdBy->isString()) t.m_createdBy = createdBy->asString();
    }
    if (const bencode::Value* creationDate = root.find("creation date")) {
        if (creationDate->isInt()) t.m_creationDate = creationDate->asInt();
    }

    t.m_name = info.at("name").asString();
    validatePathComponent(t.m_name);

    t.m_pieceLength = requirePositive(info.at("piece length").asInt(), "piece length");
    t.m_pieceHashes = splitPieceHashes(info.at("pieces").asString());

    if (const bencode::Value* files = info.find("files")) {
        // Multi-file torrent: every entry lives below a directory named `name`.
        t.m_multiFile = true;
        int64_t offset = 0;
        for (const bencode::Value& file : files->asList()) {
            FileEntry entry;
            entry.length = requirePositive(file.at("length").asInt(), "file length");
            entry.offset = offset;

            std::string path = t.m_name;
            for (const bencode::Value& component : file.at("path").asList()) {
                validatePathComponent(component.asString());
                path += '/';
                path += component.asString();
            }
            entry.path = std::move(path);
            offset += entry.length;
            t.m_files.push_back(std::move(entry));
        }
        if (t.m_files.empty()) {
            throw TorrentError("torrent: multi-file torrent with empty file list");
        }
        t.m_totalLength = offset;
    } else {
        // Single-file torrent: `name` is the file name.
        t.m_multiFile = false;
        FileEntry entry;
        entry.path = t.m_name;
        entry.length = requirePositive(info.at("length").asInt(), "file length");
        entry.offset = 0;
        t.m_totalLength = entry.length;
        t.m_files.push_back(std::move(entry));
    }

    const auto expectedPieces =
        static_cast<size_t>((t.m_totalLength + t.m_pieceLength - 1) / t.m_pieceLength);
    if (expectedPieces != t.m_pieceHashes.size()) {
        throw TorrentError("torrent: piece count does not match total length");
    }

    return t;
}

std::string TorrentFile::infoHashHex() const
{
    return util::toHex(m_infoHash.data(), m_infoHash.size());
}

int64_t TorrentFile::pieceSize(size_t index) const
{
    if (index >= m_pieceHashes.size()) {
        throw TorrentError("torrent: piece index out of range");
    }
    if (index + 1 == m_pieceHashes.size()) {
        const int64_t remainder = m_totalLength - static_cast<int64_t>(index) * m_pieceLength;
        return remainder; // Final piece: whatever is left.
    }
    return m_pieceLength;
}

} // namespace tx::torrent
