#include "torrentx/tracker/tracker_client.hpp"

#include "torrentx/torrent/bencode.hpp"
#include "torrentx/torrent/torrent_file.hpp"
#include "torrentx/util/byte_utils.hpp"
#include "torrentx/util/error.hpp"
#include "torrentx/util/logger.hpp"

#include <memory>
#include <sstream>

#include <curl/curl.h>

namespace tx::tracker {

namespace {

constexpr long kHttpTimeoutSeconds = 20;

/// curl_global_init must run exactly once per process before any easy
/// handle is created; tie it to a function-local static.
void ensureCurlInitialized()
{
    struct CurlGlobal {
        CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
        ~CurlGlobal() { curl_global_cleanup(); }
    };
    static CurlGlobal global;
}

size_t writeToString(char* data, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(data, size * nmemb);
    return size * nmemb;
}

std::string httpGet(const std::string& url)
{
    ensureCurlInitialized();

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(
        curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        throw TrackerError("libcurl: cannot create handle");
    }

    std::string body;
    char errorBuffer[CURL_ERROR_SIZE] = {};

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, kHttpTimeoutSeconds);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "TorrentX/1.0");
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, errorBuffer);

    const CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) {
        const char* detail = errorBuffer[0] ? errorBuffer : curl_easy_strerror(rc);
        throw TrackerError("tracker request failed: " + std::string(detail));
    }

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
        throw TrackerError("tracker returned HTTP " + std::to_string(status));
    }
    return body;
}

bool isHttpUrl(const std::string& url)
{
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

const char* eventName(Event event)
{
    switch (event) {
    case Event::Started:   return "started";
    case Event::Stopped:   return "stopped";
    case Event::Completed: return "completed";
    case Event::None:      break;
    }
    return nullptr;
}

std::string buildAnnounceUrl(const std::string& base, const AnnounceRequest& req)
{
    std::ostringstream url;
    url << base
        << (base.find('?') == std::string::npos ? '?' : '&')
        << "info_hash=" << util::urlEncode(req.infoHash)
        << "&peer_id=" << util::urlEncode(req.peerId)
        << "&port=" << req.port
        << "&uploaded=" << req.uploaded
        << "&downloaded=" << req.downloaded
        << "&left=" << req.left
        << "&compact=1"
        << "&numwant=" << req.numWant;
    if (const char* event = eventName(req.event)) {
        url << "&event=" << event;
    }
    return url.str();
}

/// Peers in compact form (BEP 23): 6 bytes per peer, 4 for the IPv4
/// address and 2 for the port, both big-endian.
void parseCompactPeers(const std::string& blob, std::vector<peer::PeerAddress>& out)
{
    if (blob.size() % 6 != 0) {
        throw TrackerError("tracker: compact peer list length not divisible by 6");
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(blob.data());
    for (size_t i = 0; i < blob.size(); i += 6) {
        peer::PeerAddress address;
        address.ip = std::to_string(bytes[i]) + '.' + std::to_string(bytes[i + 1]) + '.' +
                     std::to_string(bytes[i + 2]) + '.' + std::to_string(bytes[i + 3]);
        address.port = util::readU16BE(bytes + i + 4);
        if (address.port != 0) {
            out.push_back(std::move(address));
        }
    }
}

/// Original dictionary-model peer list: a list of {ip, port} dicts.
void parseDictPeers(const bencode::Value& list, std::vector<peer::PeerAddress>& out)
{
    for (const bencode::Value& entry : list.asList()) {
        peer::PeerAddress address;
        address.ip = entry.at("ip").asString();
        address.port = static_cast<uint16_t>(entry.at("port").asInt());
        if (address.port != 0) {
            out.push_back(std::move(address));
        }
    }
}

AnnounceResponse parseAnnounceResponse(const std::string& body, const std::string& url)
{
    bencode::Value root = [&] {
        try {
            return bencode::parse(body);
        } catch (const BencodeError& e) {
            throw TrackerError("tracker: response is not valid bencode (" +
                               std::string(e.what()) + ")");
        }
    }();

    if (!root.isDict()) {
        throw TrackerError("tracker: response is not a dictionary");
    }
    if (const bencode::Value* failure = root.find("failure reason")) {
        throw TrackerError("tracker rejected announce: " + failure->asString());
    }

    AnnounceResponse response;
    response.trackerUrl = url;
    if (const bencode::Value* interval = root.find("interval")) {
        if (interval->isInt() && interval->asInt() > 0) {
            response.interval = static_cast<int>(interval->asInt());
        }
    }
    if (const bencode::Value* complete = root.find("complete")) {
        if (complete->isInt()) response.seeders = complete->asInt();
    }
    if (const bencode::Value* incomplete = root.find("incomplete")) {
        if (incomplete->isInt()) response.leechers = incomplete->asInt();
    }

    const bencode::Value& peers = root.at("peers");
    if (peers.isString()) {
        parseCompactPeers(peers.asString(), response.peers);
    } else {
        parseDictPeers(peers, response.peers);
    }
    return response;
}

} // namespace

TrackerClient::TrackerClient(const torrent::TorrentFile& torrent)
    : m_tiers(torrent.announceTiers())
{
}

AnnounceResponse TrackerClient::announceToUrl(const std::string& url,
                                              const AnnounceRequest& request) const
{
    if (!isHttpUrl(url)) {
        throw TrackerError("unsupported tracker protocol (only http/https): " + url);
    }
    const std::string announceUrl = buildAnnounceUrl(url, request);
    log::debug("announce: ", announceUrl);
    return parseAnnounceResponse(httpGet(announceUrl), url);
}

AnnounceResponse TrackerClient::announce(const AnnounceRequest& request) const
{
    std::string lastFailure = "no usable tracker URL in torrent";
    for (const std::vector<std::string>& tier : m_tiers) {
        for (const std::string& url : tier) {
            if (!isHttpUrl(url)) {
                log::debug("skipping non-HTTP tracker: ", url);
                continue;
            }
            try {
                return announceToUrl(url, request);
            } catch (const Error& e) {
                log::warn("tracker ", url, " failed: ", e.what());
                lastFailure = e.what();
            }
        }
    }
    throw TrackerError("all trackers failed; last error: " + lastFailure);
}

} // namespace tx::tracker
