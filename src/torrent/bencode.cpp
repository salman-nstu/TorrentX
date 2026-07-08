#include "torrentx/torrent/bencode.hpp"

#include "torrentx/util/error.hpp"

#include <limits>

namespace tx::bencode {

int64_t Value::asInt() const
{
    if (!isInt()) {
        throw BencodeError("bencode: expected integer");
    }
    return std::get<int64_t>(m_data);
}

const std::string& Value::asString() const
{
    if (!isString()) {
        throw BencodeError("bencode: expected string");
    }
    return std::get<std::string>(m_data);
}

const Value::List& Value::asList() const
{
    if (!isList()) {
        throw BencodeError("bencode: expected list");
    }
    return std::get<List>(m_data);
}

const Value::Dict& Value::asDict() const
{
    if (!isDict()) {
        throw BencodeError("bencode: expected dictionary");
    }
    return std::get<Dict>(m_data);
}

const Value* Value::find(std::string_view key) const
{
    for (const auto& [k, v] : asDict()) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

const Value& Value::at(std::string_view key) const
{
    if (const Value* v = find(key)) {
        return *v;
    }
    throw BencodeError("bencode: missing required key '" + std::string(key) + "'");
}

/// Internal recursive-descent parser; a friend of Value so it can stamp
/// the raw byte spans onto parsed nodes.
class Parser {
public:
    explicit Parser(std::string_view data) : m_data(data) {}

    Value parseDocument()
    {
        Value value = parseValue(0);
        if (m_pos != m_data.size()) {
            throw BencodeError("bencode: trailing data after document");
        }
        return value;
    }

private:
    Value parseValue(int depth)
    {
        // A hostile input like "llllll..." would otherwise recurse without bound.
        if (depth > 64) {
            throw BencodeError("bencode: nesting too deep");
        }

        const size_t begin = m_pos;
        Value value;
        switch (peek()) {
        case 'i': value = Value(parseInt()); break;
        case 'l': value = Value(parseList(depth)); break;
        case 'd': value = Value(parseDict(depth)); break;
        default:  value = Value(parseString()); break;
        }
        value.m_rawBegin = begin;
        value.m_rawEnd = m_pos;
        return value;
    }

    int64_t parseInt()
    {
        expect('i');
        const size_t start = m_pos;
        if (peek() == '-') {
            ++m_pos;
        }
        while (peek() != 'e') {
            if (!isDigit(peek())) {
                throw BencodeError("bencode: invalid integer character");
            }
            ++m_pos;
        }
        const std::string_view digits = m_data.substr(start, m_pos - start);
        if (digits.empty() || digits == "-") {
            throw BencodeError("bencode: empty integer");
        }
        // Reject leading zeros ("03") and negative zero, as required by the spec.
        if (digits[0] == '0' && digits.size() > 1) {
            throw BencodeError("bencode: integer with leading zero");
        }
        if (digits.size() > 1 && digits[0] == '-' && digits[1] == '0') {
            throw BencodeError("bencode: negative zero or leading zero");
        }
        expect('e');

        int64_t result = 0;
        const bool negative = digits[0] == '-';
        for (size_t i = negative ? 1 : 0; i < digits.size(); ++i) {
            if (result > (std::numeric_limits<int64_t>::max() - (digits[i] - '0')) / 10) {
                throw BencodeError("bencode: integer overflow");
            }
            result = result * 10 + (digits[i] - '0');
        }
        return negative ? -result : result;
    }

    std::string parseString()
    {
        size_t length = 0;
        if (!isDigit(peek())) {
            throw BencodeError("bencode: expected string length");
        }
        while (peek() != ':') {
            if (!isDigit(peek())) {
                throw BencodeError("bencode: invalid string length");
            }
            if (length > m_data.size()) {
                throw BencodeError("bencode: string length exceeds input");
            }
            length = length * 10 + static_cast<size_t>(peek() - '0');
            ++m_pos;
        }
        expect(':');
        if (length > m_data.size() - m_pos) {
            throw BencodeError("bencode: string extends past end of input");
        }
        std::string result(m_data.substr(m_pos, length));
        m_pos += length;
        return result;
    }

    Value::List parseList(int depth)
    {
        expect('l');
        Value::List items;
        while (peek() != 'e') {
            items.push_back(parseValue(depth + 1));
        }
        expect('e');
        return items;
    }

    Value::Dict parseDict(int depth)
    {
        expect('d');
        Value::Dict entries;
        while (peek() != 'e') {
            std::string key = parseString();
            entries.emplace_back(std::move(key), parseValue(depth + 1));
        }
        expect('e');
        return entries;
    }

    static bool isDigit(char c) { return c >= '0' && c <= '9'; }

    char peek() const
    {
        if (m_pos >= m_data.size()) {
            throw BencodeError("bencode: unexpected end of input");
        }
        return m_data[m_pos];
    }

    void expect(char c)
    {
        if (peek() != c) {
            throw BencodeError(std::string("bencode: expected '") + c + "'");
        }
        ++m_pos;
    }

    std::string_view m_data;
    size_t m_pos = 0;
};

namespace {

void encodeTo(const Value& value, std::string& out)
{
    if (value.isInt()) {
        out += 'i';
        out += std::to_string(value.asInt());
        out += 'e';
    } else if (value.isString()) {
        const std::string& s = value.asString();
        out += std::to_string(s.size());
        out += ':';
        out += s;
    } else if (value.isList()) {
        out += 'l';
        for (const Value& item : value.asList()) {
            encodeTo(item, out);
        }
        out += 'e';
    } else {
        out += 'd';
        for (const auto& [key, item] : value.asDict()) {
            out += std::to_string(key.size());
            out += ':';
            out += key;
            encodeTo(item, out);
        }
        out += 'e';
    }
}

} // namespace

Value parse(std::string_view data)
{
    return Parser(data).parseDocument();
}

std::string encode(const Value& value)
{
    std::string out;
    encodeTo(value, out);
    return out;
}

} // namespace tx::bencode
