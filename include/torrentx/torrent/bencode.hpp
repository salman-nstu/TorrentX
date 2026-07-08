#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tx::bencode {

/// A parsed bencode value: integer, byte string, list or dictionary.
///
/// Dictionaries are stored as ordered key/value vectors rather than a map:
/// bencode requires keys in sorted order, keeping the original order makes
/// encode(parse(x)) == x for spec-conforming input, and torrent dictionaries
/// are small enough that linear lookup is never a bottleneck.
class Value {
public:
    using List = std::vector<Value>;
    using Dict = std::vector<std::pair<std::string, Value>>;

    Value() : m_data(int64_t{0}) {}
    explicit Value(int64_t v) : m_data(v) {}
    explicit Value(std::string v) : m_data(std::move(v)) {}
    explicit Value(List v) : m_data(std::move(v)) {}
    explicit Value(Dict v) : m_data(std::move(v)) {}

    bool isInt() const    { return std::holds_alternative<int64_t>(m_data); }
    bool isString() const { return std::holds_alternative<std::string>(m_data); }
    bool isList() const   { return std::holds_alternative<List>(m_data); }
    bool isDict() const   { return std::holds_alternative<Dict>(m_data); }

    /// Accessors throw tx::BencodeError when the type does not match.
    int64_t asInt() const;
    const std::string& asString() const;
    const List& asList() const;
    const Dict& asDict() const;

    /// Dictionary lookup; returns nullptr when the key is absent.
    const Value* find(std::string_view key) const;

    /// Like find(), but throws tx::BencodeError when the key is absent.
    const Value& at(std::string_view key) const;

    /// Byte range [begin, end) this value occupied in the parsed input.
    /// Used to hash the exact original bytes of the `info` dictionary.
    size_t rawBegin() const { return m_rawBegin; }
    size_t rawEnd() const   { return m_rawEnd; }

private:
    friend class Parser;

    std::variant<int64_t, std::string, List, Dict> m_data;
    size_t m_rawBegin = 0;
    size_t m_rawEnd = 0;
};

/// Parses a complete bencode document; trailing bytes are rejected.
/// Throws tx::BencodeError on malformed input.
Value parse(std::string_view data);

/// Serializes a value back to its bencode representation.
std::string encode(const Value& value);

} // namespace tx::bencode
