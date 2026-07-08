#include "torrentx/protocol/handshake.hpp"

#include "torrentx/util/error.hpp"

#include <algorithm>
#include <cstring>

namespace tx::protocol {

constexpr char Handshake::kProtocolString[];

std::array<uint8_t, Handshake::kSize> Handshake::serialize() const
{
    std::array<uint8_t, kSize> out{};
    out[0] = 19;
    std::memcpy(out.data() + 1, kProtocolString, 19);
    // Bytes 20..27 are the reserved extension bits; we advertise none.
    std::copy(infoHash.begin(), infoHash.end(), out.begin() + 28);
    std::copy(peerId.begin(), peerId.end(), out.begin() + 48);
    return out;
}

Handshake Handshake::parse(const std::array<uint8_t, kSize>& raw)
{
    if (raw[0] != 19 || std::memcmp(raw.data() + 1, kProtocolString, 19) != 0) {
        throw ProtocolError("handshake: peer does not speak BitTorrent protocol");
    }
    Handshake hs;
    std::copy_n(raw.begin() + 28, 20, hs.infoHash.begin());
    std::copy_n(raw.begin() + 48, 20, hs.peerId.begin());
    return hs;
}

Handshake Handshake::make(const crypto::Sha1Digest& infoHash, const std::string& peerId)
{
    if (peerId.size() != 20) {
        throw ProtocolError("handshake: peer id must be exactly 20 bytes");
    }
    Handshake hs;
    hs.infoHash = infoHash;
    std::copy_n(reinterpret_cast<const uint8_t*>(peerId.data()), 20, hs.peerId.begin());
    return hs;
}

} // namespace tx::protocol
