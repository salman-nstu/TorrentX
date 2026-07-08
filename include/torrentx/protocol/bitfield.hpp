#pragma once

#include <cstdint>
#include <vector>

namespace tx::protocol {

/// Piece-availability bitmap exchanged in the `bitfield` message.
/// Bit 0 of byte 0 is the most significant bit (piece 0), per BEP 3.
class Bitfield {
public:
    Bitfield() = default;

    explicit Bitfield(size_t pieceCount)
        : m_pieceCount(pieceCount)
        , m_bytes((pieceCount + 7) / 8, 0)
    {
    }

    /// Interprets a received bitfield payload for a known piece count.
    /// Returns false when the payload length is wrong or spare bits are
    /// set — both protocol violations.
    bool assign(const std::vector<uint8_t>& payload, size_t pieceCount)
    {
        if (payload.size() != (pieceCount + 7) / 8) {
            return false;
        }
        const size_t spareBits = payload.size() * 8 - pieceCount;
        if (spareBits > 0) {
            const uint8_t spareMask = static_cast<uint8_t>((1u << spareBits) - 1);
            if ((payload.back() & spareMask) != 0) {
                return false;
            }
        }
        m_bytes = payload;
        m_pieceCount = pieceCount;
        return true;
    }

    bool has(size_t piece) const
    {
        if (piece >= m_pieceCount) {
            return false;
        }
        return (m_bytes[piece / 8] >> (7 - piece % 8)) & 1;
    }

    void set(size_t piece)
    {
        if (piece < m_pieceCount) {
            m_bytes[piece / 8] |= static_cast<uint8_t>(1u << (7 - piece % 8));
        }
    }

    size_t count() const
    {
        size_t total = 0;
        for (size_t i = 0; i < m_pieceCount; ++i) {
            total += has(i) ? 1 : 0;
        }
        return total;
    }

    size_t pieceCount() const { return m_pieceCount; }
    bool empty() const { return m_pieceCount == 0; }

private:
    size_t m_pieceCount = 0;
    std::vector<uint8_t> m_bytes;
};

} // namespace tx::protocol
