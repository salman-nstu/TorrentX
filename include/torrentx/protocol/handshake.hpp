#pragma once

#include "torrentx/crypto/sha1.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace tx::protocol {

/// The fixed-size 68-byte BitTorrent handshake (BEP 3):
///   <pstrlen=19><pstr="BitTorrent protocol"><8 reserved><info_hash><peer_id>
struct Handshake {
    static constexpr size_t kSize = 68;
    static constexpr char kProtocolString[] = "BitTorrent protocol";

    crypto::Sha1Digest infoHash{};
    std::array<uint8_t, 20> peerId{};

    /// Serializes to the 68-byte wire format (reserved bits all zero).
    std::array<uint8_t, kSize> serialize() const;

    /// Parses and validates a received handshake.
    /// Throws tx::ProtocolError when the protocol string is wrong.
    static Handshake parse(const std::array<uint8_t, kSize>& raw);

    /// Builds our outgoing handshake from an info hash and a 20-char peer id.
    static Handshake make(const crypto::Sha1Digest& infoHash, const std::string& peerId);
};

} // namespace tx::protocol
