#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tx::util {

/// Lowercase hex representation of a byte buffer, e.g. "6a3f...".
std::string toHex(const uint8_t* data, size_t size);

template <typename Container>
std::string toHex(const Container& bytes)
{
    return toHex(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

/// Percent-encodes arbitrary binary data for use in a URL query string.
/// Unreserved characters (RFC 3986) pass through, everything else becomes %XX.
std::string urlEncode(const uint8_t* data, size_t size);

template <typename Container>
std::string urlEncode(const Container& bytes)
{
    return urlEncode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

/// Big-endian (network order) integer packing used throughout the wire protocol.
uint32_t readU32BE(const uint8_t* data);
uint16_t readU16BE(const uint8_t* data);
void writeU32BE(uint8_t* out, uint32_t value);
void appendU32BE(std::vector<uint8_t>& out, uint32_t value);

/// Human-readable size, e.g. "698.4 MiB".
std::string formatSize(uint64_t bytes);

/// "HH:MM:SS" from a duration in seconds; "--:--" when unknown.
std::string formatEta(int64_t seconds);

/// Generates the 20-byte peer id: "-TX0100-" followed by 12 random
/// alphanumeric characters (Azureus-style, BEP 20).
std::string generatePeerId();

} // namespace tx::util
