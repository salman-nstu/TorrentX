#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace tx::crypto {

/// A raw 20-byte SHA-1 digest as used for info hashes and piece hashes.
using Sha1Digest = std::array<uint8_t, 20>;

/// Computes SHA-1 of a buffer using OpenSSL's EVP interface.
Sha1Digest sha1(const uint8_t* data, size_t size);

inline Sha1Digest sha1(const std::string& data)
{
    return sha1(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

} // namespace tx::crypto
