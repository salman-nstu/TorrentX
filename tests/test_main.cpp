// Unit tests for the TorrentX engine. Deliberately dependency-free: a
// tiny CHECK macro set keeps the project's footprint at libcurl+OpenSSL.

#include "torrentx/crypto/sha1.hpp"
#include "torrentx/download/file_storage.hpp"
#include "torrentx/download/piece_manager.hpp"
#include "torrentx/protocol/bitfield.hpp"
#include "torrentx/protocol/handshake.hpp"
#include "torrentx/protocol/message.hpp"
#include "torrentx/torrent/bencode.hpp"
#include "torrentx/torrent/torrent_file.hpp"
#include "torrentx/util/byte_utils.hpp"
#include "torrentx/util/error.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_THROWS(expression, ExceptionType)                            \
    do {                                                                   \
        ++g_checks;                                                        \
        bool caught = false;                                               \
        try {                                                              \
            (void)(expression);                                            \
        } catch (const ExceptionType&) {                                   \
            caught = true;                                                 \
        }                                                                  \
        if (!caught) {                                                     \
            std::printf("FAIL %s:%d: expected %s from %s\n",               \
                        __FILE__, __LINE__, #ExceptionType, #expression);  \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ---------------------------------------------------------------- crypto

void testSha1()
{
    // Canonical test vectors from FIPS 180-1.
    CHECK(tx::util::toHex(tx::crypto::sha1(std::string("abc"))) ==
          "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(tx::util::toHex(tx::crypto::sha1(std::string(""))) ==
          "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(tx::util::toHex(tx::crypto::sha1(std::string(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

// ------------------------------------------------------------------ util

void testByteUtils()
{
    const uint8_t bytes[] = { 0x00, 0x9a, 0xff };
    CHECK(tx::util::toHex(bytes, 3) == "009aff");

    // RFC 3986 unreserved characters survive, binary gets escaped.
    const uint8_t raw[] = { 'A', 'z', '0', '~', '.', ' ', 0x00, 0xff };
    CHECK(tx::util::urlEncode(raw, 8) == "Az0~.%20%00%FF");

    uint8_t buf[4];
    tx::util::writeU32BE(buf, 0x01020304u);
    CHECK(buf[0] == 1 && buf[1] == 2 && buf[2] == 3 && buf[3] == 4);
    CHECK(tx::util::readU32BE(buf) == 0x01020304u);

    const uint8_t port[] = { 0x1a, 0xe1 };
    CHECK(tx::util::readU16BE(port) == 6881);

    const std::string peerId = tx::util::generatePeerId();
    CHECK(peerId.size() == 20);
    CHECK(peerId.rfind("-TX0100-", 0) == 0);
}

// --------------------------------------------------------------- bencode

void testBencode()
{
    using tx::bencode::parse;

    CHECK(parse("i42e").asInt() == 42);
    CHECK(parse("i-7e").asInt() == -7);
    CHECK(parse("i0e").asInt() == 0);
    CHECK(parse("4:spam").asString() == "spam");
    CHECK(parse("0:").asString().empty());

    const auto list = parse("l4:spami42ee");
    CHECK(list.asList().size() == 2);
    CHECK(list.asList()[0].asString() == "spam");
    CHECK(list.asList()[1].asInt() == 42);

    const auto dict = parse("d3:bar4:spam3:fooi42ee");
    CHECK(dict.at("bar").asString() == "spam");
    CHECK(dict.at("foo").asInt() == 42);
    CHECK(dict.find("baz") == nullptr);

    // Errors the spec requires us to reject.
    CHECK_THROWS(parse("i03e"), tx::BencodeError);      // leading zero
    CHECK_THROWS(parse("i-0e"), tx::BencodeError);      // negative zero
    CHECK_THROWS(parse("i42ex"), tx::BencodeError);     // trailing data
    CHECK_THROWS(parse("5:spam"), tx::BencodeError);    // short string
    CHECK_THROWS(parse("l4:spam"), tx::BencodeError);   // unterminated list
    CHECK_THROWS(parse(""), tx::BencodeError);          // empty input
    CHECK_THROWS(parse("d3:fooe"), tx::BencodeError);   // key without value
    CHECK_THROWS(parse("i9223372036854775808e"), tx::BencodeError); // overflow

    // encode(parse(x)) == x for canonical input.
    const std::string canonical = "d3:bar4:spam3:fooli1ei2eee";
    CHECK(tx::bencode::encode(parse(canonical)) == canonical);

    // Raw spans must point at the exact original bytes (info-hash basis).
    const std::string doc = "d4:infod3:fooi42eee";
    const auto root = parse(doc);
    const auto& info = root.at("info");
    CHECK(doc.substr(info.rawBegin(), info.rawEnd() - info.rawBegin()) ==
          "d3:fooi42ee");
}

// ---------------------------------------------------- torrent construction

/// Builds a syntactically valid single-file torrent for `payload`.
std::string makeSingleFileTorrent(const std::string& payload,
                                  int64_t pieceLength,
                                  const std::string& name)
{
    std::string pieces;
    for (size_t offset = 0; offset < payload.size();
         offset += static_cast<size_t>(pieceLength)) {
        const size_t n = std::min(static_cast<size_t>(pieceLength),
                                  payload.size() - offset);
        const auto digest = tx::crypto::sha1(
            reinterpret_cast<const uint8_t*>(payload.data()) + offset, n);
        pieces.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    }

    std::string info;
    info += "d6:lengthi" + std::to_string(payload.size()) + "e";
    info += "4:name" + std::to_string(name.size()) + ":" + name;
    info += "12:piece lengthi" + std::to_string(pieceLength) + "e";
    info += "6:pieces" + std::to_string(pieces.size()) + ":" + pieces;
    info += "e";

    return "d8:announce31:http://tracker.example/announce4:info" + info + "e";
}

void testTorrentFile()
{
    const std::string payload = "hello, torrent world! this payload spans pieces.";
    const std::string buffer = makeSingleFileTorrent(payload, 16, "hello.txt");

    const auto torrent = tx::torrent::TorrentFile::loadFromBuffer(buffer);
    CHECK(torrent.name() == "hello.txt");
    CHECK(torrent.announce() == "http://tracker.example/announce");
    CHECK(torrent.totalLength() == static_cast<int64_t>(payload.size()));
    CHECK(torrent.pieceLength() == 16);
    CHECK(torrent.pieceCount() == (payload.size() + 15) / 16);
    CHECK(!torrent.isMultiFile());
    CHECK(torrent.files().size() == 1);
    CHECK(torrent.files()[0].path == "hello.txt");
    CHECK(torrent.pieceSize(torrent.pieceCount() - 1) ==
          static_cast<int64_t>(payload.size() % 16 == 0 ? 16 : payload.size() % 16));

    // The info hash must cover exactly the info dictionary's bytes.
    const size_t infoPos = buffer.find("4:info") + 6;
    const auto expected = tx::crypto::sha1(
        reinterpret_cast<const uint8_t*>(buffer.data()) + infoPos,
        buffer.size() - infoPos - 1); // trailing 'e' of the root dict
    CHECK(torrent.infoHash() == expected);

    // Multi-file torrent with directory structure and global offsets.
    const std::string multi =
        "d8:announce9:http://t/4:info"
        "d5:filesl"
        "d6:lengthi5e4:pathl1:a3:b.xee"
        "d6:lengthi7e4:pathl1:c1:d3:e.yee"
        "e"
        "4:name3:dir"
        "12:piece lengthi16384e"
        "6:pieces20:aaaaaaaaaaaaaaaaaaaa"
        "e" "e";
    const auto multiTorrent = tx::torrent::TorrentFile::loadFromBuffer(multi);
    CHECK(multiTorrent.isMultiFile());
    CHECK(multiTorrent.totalLength() == 12);
    CHECK(multiTorrent.files().size() == 2);
    CHECK(multiTorrent.files()[0].path == "dir/a/b.x");
    CHECK(multiTorrent.files()[1].path == "dir/c/d/e.y");
    CHECK(multiTorrent.files()[1].offset == 5);

    // Directory traversal must be rejected.
    const std::string evil =
        "d8:announce9:http://t/4:info"
        "d5:filesl"
        "d6:lengthi5e4:pathl2:..7:evil.exee"
        "e"
        "4:name3:dir"
        "12:piece lengthi16384e"
        "6:pieces20:aaaaaaaaaaaaaaaaaaaa"
        "e" "e";
    CHECK_THROWS(tx::torrent::TorrentFile::loadFromBuffer(evil), tx::TorrentError);

    // Piece count must match the payload size.
    CHECK_THROWS(tx::torrent::TorrentFile::loadFromBuffer(
                     makeSingleFileTorrent("xx", 16, "x") + " "),
                 tx::BencodeError); // trailing byte
}

// -------------------------------------------------------------- protocol

void testHandshake()
{
    tx::crypto::Sha1Digest infoHash{};
    for (size_t i = 0; i < infoHash.size(); ++i) {
        infoHash[i] = static_cast<uint8_t>(i);
    }

    const auto ours = tx::protocol::Handshake::make(infoHash, "-TX0100-abcdefghijkl");
    const auto raw = ours.serialize();
    CHECK(raw.size() == 68);
    CHECK(raw[0] == 19);
    CHECK(std::string(raw.begin() + 1, raw.begin() + 20) == "BitTorrent protocol");

    const auto parsed = tx::protocol::Handshake::parse(raw);
    CHECK(parsed.infoHash == infoHash);
    CHECK(std::string(parsed.peerId.begin(), parsed.peerId.end()) ==
          "-TX0100-abcdefghijkl");

    auto corrupted = raw;
    corrupted[5] = 'X';
    CHECK_THROWS(tx::protocol::Handshake::parse(corrupted), tx::ProtocolError);

    CHECK_THROWS(tx::protocol::Handshake::make(infoHash, "short"), tx::ProtocolError);
}

void testBitfield()
{
    tx::protocol::Bitfield field(10);
    CHECK(!field.has(3));
    field.set(3);
    field.set(9);
    CHECK(field.has(3));
    CHECK(field.has(9));
    CHECK(!field.has(10)); // out of range
    CHECK(field.count() == 2);

    tx::protocol::Bitfield received;
    // 10 pieces => 2 bytes; 0b10100000 0b01000000 => pieces 0, 2, 9.
    CHECK(received.assign({ 0xa0, 0x40 }, 10));
    CHECK(received.has(0) && received.has(2) && received.has(9));
    CHECK(received.count() == 3);

    CHECK(!received.assign({ 0xa0 }, 10));        // wrong length
    CHECK(!received.assign({ 0xa0, 0x41 }, 10));  // spare bit set
}

void testMessages()
{
    const tx::protocol::BlockRequest request{ 7, 16384, 16384 };
    const auto payload = tx::protocol::encodeRequest(request);
    CHECK(payload.size() == 12);
    CHECK(tx::util::readU32BE(payload.data()) == 7);
    CHECK(tx::util::readU32BE(payload.data() + 4) == 16384);
    CHECK(tx::util::readU32BE(payload.data() + 8) == 16384);

    tx::protocol::Message have;
    have.type = tx::protocol::MessageType::Have;
    have.payload = { 0, 0, 0, 5 };
    CHECK(tx::protocol::decodeHave(have) == 5);
    have.payload.push_back(0);
    CHECK_THROWS(tx::protocol::decodeHave(have), tx::ProtocolError);

    tx::protocol::Message piece;
    piece.type = tx::protocol::MessageType::Piece;
    piece.payload = { 0, 0, 0, 2, 0, 0, 0x40, 0, 'd', 'a', 't', 'a' };
    const auto block = tx::protocol::decodePiece(std::move(piece));
    CHECK(block.pieceIndex == 2);
    CHECK(block.begin == 16384);
    CHECK(block.data.size() == 4);

    tx::protocol::Message truncated;
    truncated.type = tx::protocol::MessageType::Piece;
    truncated.payload = { 0, 0, 0 };
    CHECK_THROWS(tx::protocol::decodePiece(std::move(truncated)), tx::ProtocolError);
}

// ------------------------------------------- piece manager + file storage

/// End-to-end without a network: feed blocks into the PieceManager and
/// confirm verified data lands in the file, then re-verify from disk.
void testPieceManagerRoundTrip()
{
    namespace fs = std::filesystem;

    // Payload of ~2.5 blocks so we get a full piece plus a short tail.
    std::string payload;
    payload.reserve(40000);
    std::mt19937 gen(1234);
    for (size_t i = 0; i < 40000; ++i) {
        payload.push_back(static_cast<char>(gen() & 0xff));
    }

    const int64_t pieceLength = tx::download::PieceManager::kBlockSize; // 1 block/piece
    const std::string buffer = makeSingleFileTorrent(payload, pieceLength, "data.bin");
    const auto torrent = tx::torrent::TorrentFile::loadFromBuffer(buffer);
    CHECK(torrent.pieceCount() == 3);

    const fs::path dir = fs::temp_directory_path() / "torrentx-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    {
        tx::download::FileStorage storage(torrent, dir);
        storage.open(tx::download::FileStorage::Mode::ReadWrite);
        CHECK(!storage.hasPreexistingData());

        tx::download::PieceManager pieces(torrent, storage);
        CHECK(!pieces.isComplete());
        CHECK(pieces.totalBytes() == 40000);

        // A peer owning everything asks for work until nothing is left.
        tx::protocol::Bitfield all(torrent.pieceCount());
        for (size_t i = 0; i < torrent.pieceCount(); ++i) {
            all.set(i);
        }
        CHECK(pieces.isInteresting(all));

        while (auto request = pieces.nextRequest(all, /*peerKey=*/1)) {
            tx::protocol::PieceBlock block;
            block.pieceIndex = request->pieceIndex;
            block.begin = request->begin;
            const size_t offset = static_cast<size_t>(request->pieceIndex) *
                                  static_cast<size_t>(pieceLength) + request->begin;
            block.data.assign(payload.begin() + offset,
                              payload.begin() + offset + request->length);
            pieces.onBlockReceived(1, block);
        }

        CHECK(pieces.isComplete());
        CHECK(pieces.completedPieces() == 3);
        CHECK(pieces.verifiedBytes() == 40000);
        CHECK(pieces.hashFailures() == 0);
        CHECK(!pieces.isInteresting(all)); // nothing left to want
    }

    // The file on disk must be byte-identical to the payload.
    {
        std::ifstream in(dir / "data.bin", std::ios::binary);
        std::string written((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        CHECK(written == payload);
    }

    // Fresh storage over the same directory: resume sees every piece.
    {
        tx::download::FileStorage storage(torrent, dir);
        storage.open(tx::download::FileStorage::Mode::ReadOnly);
        CHECK(storage.hasPreexistingData());

        tx::download::PieceManager pieces(torrent, storage);
        CHECK(pieces.verifyExisting() == 3);
        CHECK(pieces.isComplete());
    }

    // Corrupt one byte: exactly that piece must fail verification.
    {
        std::fstream f(dir / "data.bin", std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(0);
        char c;
        f.seekg(0);
        f.get(c);
        f.seekp(0);
        f.put(static_cast<char>(c ^ 0xff));
    }
    {
        tx::download::FileStorage storage(torrent, dir);
        storage.open(tx::download::FileStorage::Mode::ReadOnly);
        tx::download::PieceManager pieces(torrent, storage);
        CHECK(pieces.verifyExisting() == 2);
        CHECK(!pieces.isComplete());
    }

    fs::remove_all(dir);
}

} // namespace

int main()
{
    testSha1();
    testByteUtils();
    testBencode();
    testTorrentFile();
    testHandshake();
    testBitfield();
    testMessages();
    testPieceManagerRoundTrip();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
