#include "torrentx/download/file_storage.hpp"

#include "torrentx/util/error.hpp"

#include <algorithm>

namespace tx::download {

namespace fs = std::filesystem;

FileStorage::FileStorage(const torrent::TorrentFile& torrent, const fs::path& outputDir)
    : m_outputDir(outputDir)
    , m_totalLength(torrent.totalLength())
{
    for (const torrent::FileEntry& file : torrent.files()) {
        Segment segment;
        // FileEntry::path is validated during parsing (no "..", no separators
        // inside components), so joining it under outputDir is safe.
        segment.path = m_outputDir / fs::path(file.path, fs::path::generic_format);
        segment.globalOffset = file.offset;
        segment.length = file.length;
        m_segments.push_back(std::move(segment));
    }
}

void FileStorage::open(Mode mode)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_opened) {
        return;
    }

    for (Segment& segment : m_segments) {
        std::error_code ec;
        const bool exists = fs::exists(segment.path, ec);
        if (exists && fs::file_size(segment.path, ec) > 0) {
            m_hadExistingData = true;
        }

        if (mode == Mode::ReadOnly) {
            if (exists) {
                segment.stream.open(segment.path, std::ios::binary | std::ios::in);
            }
            continue; // Missing files simply stay unopened.
        }

        fs::create_directories(segment.path.parent_path(), ec);
        if (!exists) {
            // Create the file without truncating anything that exists.
            std::ofstream create(segment.path, std::ios::binary);
            if (!create) {
                throw StorageError("cannot create file: " + segment.path.string());
            }
        }

        segment.stream.open(segment.path,
                            std::ios::binary | std::ios::in | std::ios::out);
        if (!segment.stream) {
            throw StorageError("cannot open file: " + segment.path.string());
        }

        // Extend short files to their final size so random-access writes
        // anywhere in the file are valid.
        segment.stream.seekp(0, std::ios::end);
        const int64_t current = segment.stream.tellp();
        if (current < segment.length) {
            segment.stream.seekp(segment.length - 1);
            segment.stream.put('\0');
            segment.stream.flush();
            if (!segment.stream) {
                throw StorageError("cannot allocate space for: " + segment.path.string());
            }
        }
    }
    m_opened = true;
}

template <typename Op>
void FileStorage::forEachSegment(int64_t offset, int64_t size, Op&& op)
{
    if (offset < 0 || size < 0 || offset + size > m_totalLength) {
        throw StorageError("storage access out of bounds");
    }

    int64_t bufferOffset = 0;
    for (Segment& segment : m_segments) {
        const int64_t segmentEnd = segment.globalOffset + segment.length;
        if (segmentEnd <= offset) {
            continue;
        }
        if (segment.globalOffset >= offset + size) {
            break;
        }
        const int64_t chunkStart = std::max(offset, segment.globalOffset);
        const int64_t chunkEnd = std::min(offset + size, segmentEnd);
        op(segment, chunkStart - segment.globalOffset, bufferOffset, chunkEnd - chunkStart);
        bufferOffset += chunkEnd - chunkStart;
    }
}

void FileStorage::write(int64_t offset, const uint8_t* data, int64_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened) {
        throw StorageError("storage not opened");
    }
    forEachSegment(offset, size,
        [&](Segment& segment, int64_t fileOffset, int64_t bufferOffset, int64_t chunk) {
            if (!segment.stream.is_open()) {
                throw StorageError("write to unopened file: " + segment.path.string());
            }
            segment.stream.clear();
            segment.stream.seekp(fileOffset);
            segment.stream.write(reinterpret_cast<const char*>(data + bufferOffset), chunk);
            if (!segment.stream) {
                throw StorageError("write failed: " + segment.path.string());
            }
        });
}

bool FileStorage::read(int64_t offset, uint8_t* out, int64_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened) {
        throw StorageError("storage not opened");
    }
    bool complete = true;
    forEachSegment(offset, size,
        [&](Segment& segment, int64_t fileOffset, int64_t bufferOffset, int64_t chunk) {
            if (!segment.stream.is_open()) {
                complete = false;
                return;
            }
            segment.stream.clear();
            segment.stream.seekg(fileOffset);
            segment.stream.read(reinterpret_cast<char*>(out + bufferOffset), chunk);
            if (segment.stream.gcount() != chunk) {
                complete = false;
                segment.stream.clear();
            }
        });
    return complete;
}

} // namespace tx::download
