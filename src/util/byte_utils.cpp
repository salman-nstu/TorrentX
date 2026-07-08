#include "torrentx/util/byte_utils.hpp"

#include <cstdio>
#include <random>

namespace tx::util {

std::string toHex(const uint8_t* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

std::string urlEncode(const uint8_t* data, size_t size)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(size * 3);
    for (size_t i = 0; i < size; ++i) {
        const uint8_t c = data[i];
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0x0f]);
        }
    }
    return out;
}

uint32_t readU32BE(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8)  |
            static_cast<uint32_t>(data[3]);
}

uint16_t readU16BE(const uint8_t* data)
{
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

void writeU32BE(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

void appendU32BE(std::vector<uint8_t>& out, uint32_t value)
{
    uint8_t buf[4];
    writeU32BE(buf, value);
    out.insert(out.end(), buf, buf + 4);
}

std::string formatSize(uint64_t bytes)
{
    static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
    }
    return buffer;
}

std::string formatEta(int64_t seconds)
{
    if (seconds < 0 || seconds > 99 * 3600) {
        return "--:--";
    }
    char buffer[16];
    if (seconds >= 3600) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld",
                      static_cast<long long>(seconds / 3600),
                      static_cast<long long>((seconds / 60) % 60),
                      static_cast<long long>(seconds % 60));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld",
                      static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    }
    return buffer;
}

std::string generatePeerId()
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    std::string id = "-TX0100-";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> pick(0, sizeof(alphabet) - 2);
    while (id.size() < 20) {
        id.push_back(alphabet[pick(gen)]);
    }
    return id;
}

} // namespace tx::util
