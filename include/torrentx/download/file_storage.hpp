#pragma once

#include "torrentx/torrent/torrent_file.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace tx::download {

/// Maps the torrent's single concatenated byte stream onto the payload
/// files on disk. Pieces are hashed over this stream, so a piece can span
/// several files in a multi-file torrent; this class hides that split.
///
/// Thread-safe: all reads and writes are serialized by an internal mutex.
class FileStorage {
public:
    enum class Mode {
        ReadWrite, ///< Create missing files and extend them to final size.
        ReadOnly,  ///< Never touch the disk layout; used by `verify`.
    };

    FileStorage(const torrent::TorrentFile& torrent,
                const std::filesystem::path& outputDir);

    /// Opens the payload files. In ReadWrite mode missing files are
    /// created and short files extended, preserving existing contents so
    /// a partial download can be resumed.
    void open(Mode mode);

    /// True when at least one payload file already held data before
    /// open() ran — i.e. resume verification is worth doing.
    bool hasPreexistingData() const { return m_hadExistingData; }

    /// Writes `size` bytes at global stream offset `offset`.
    void write(int64_t offset, const uint8_t* data, int64_t size);

    /// Reads `size` bytes at global offset; returns false when the data
    /// does not exist on disk (missing or short file).
    bool read(int64_t offset, uint8_t* out, int64_t size);

    const std::filesystem::path& outputDir() const { return m_outputDir; }

private:
    struct Segment {
        std::filesystem::path path;
        int64_t globalOffset = 0;
        int64_t length = 0;
        std::fstream stream;
    };

    /// Invokes op(segment, fileOffset, bufferOffset, chunkSize) for every
    /// file segment overlapping [offset, offset+size).
    template <typename Op>
    void forEachSegment(int64_t offset, int64_t size, Op&& op);

    std::mutex m_mutex;
    std::filesystem::path m_outputDir;
    std::vector<Segment> m_segments;
    int64_t m_totalLength = 0;
    bool m_opened = false;
    bool m_hadExistingData = false;
};

} // namespace tx::download
