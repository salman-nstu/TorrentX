#pragma once

#include "torrentx/crypto/sha1.hpp"
#include "torrentx/peer/peer_address.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tx::torrent { class TorrentFile; }

namespace tx::tracker {

/// Announce event as defined by BEP 3.
enum class Event {
    None,
    Started,
    Stopped,
    Completed,
};

/// Parameters for one announce request.
struct AnnounceRequest {
    crypto::Sha1Digest infoHash{};
    std::string peerId;       ///< Exactly 20 bytes.
    uint16_t port = 6881;
    int64_t uploaded = 0;
    int64_t downloaded = 0;
    int64_t left = 0;
    Event event = Event::None;
    int numWant = 50;
};

/// Parsed tracker response.
struct AnnounceResponse {
    int interval = 1800;               ///< Seconds until the next announce.
    int64_t seeders = -1;              ///< -1 when the tracker did not say.
    int64_t leechers = -1;
    std::vector<peer::PeerAddress> peers;
    std::string trackerUrl;            ///< The URL that produced this response.
};

/// HTTP(S) tracker client built on libcurl.
///
/// announce() walks the torrent's tracker tiers (BEP 12) and returns the
/// first successful response. UDP trackers are skipped: TorrentX speaks
/// the HTTP tracker protocol only, per the project requirements.
class TrackerClient {
public:
    explicit TrackerClient(const torrent::TorrentFile& torrent);

    AnnounceResponse announce(const AnnounceRequest& request) const;

    /// Announces against one specific tracker URL.
    AnnounceResponse announceToUrl(const std::string& url,
                                   const AnnounceRequest& request) const;

private:
    std::vector<std::vector<std::string>> m_tiers;
};

} // namespace tx::tracker
