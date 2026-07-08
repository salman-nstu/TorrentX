# TorrentX Architecture

TorrentX is organized as a small set of layered modules. Each module lives
in its own directory under `include/torrentx/` (headers) and `src/`
(implementations); dependencies point strictly downward.

```
                 ┌─────────────────────────────┐
                 │            cli               │  main, argument parsing,
                 │  download · info · peers ·   │  signal handling
                 │           verify             │
                 └──────────────┬──────────────┘
                                │
                 ┌──────────────▼──────────────┐
                 │     download::DownloadEngine │  orchestration loop
                 └──┬───────────┬───────────┬──┘
                    │           │           │
      ┌─────────────▼──┐  ┌─────▼──────┐  ┌─▼──────────────┐
      │ tracker::       │  │ peer::     │  │ download::     │
      │ TrackerClient   │  │ PeerManager│  │ PieceManager   │
      │ (libcurl)       │  │ + workers  │  │ + FileStorage  │
      └───────┬────────┘  └─────┬──────┘  └───┬────────────┘
              │                 │             │
      ┌───────▼────────┐  ┌─────▼──────────┐ ┌▼───────────────┐
      │ torrent::       │  │ peer::         │ │ crypto::sha1   │
      │ bencode,        │  │ PeerConnection │ │ (OpenSSL EVP)  │
      │ TorrentFile     │  │ + protocol::*  │ └────────────────┘
      └────────────────┘  └────────────────┘
                 util:: error, logger, tcp_socket, byte_utils
```

## Modules

### `torrent` — metainfo parsing
- **`bencode`**: recursive-descent parser and encoder for the bencode
  format. Dictionaries are kept as *ordered* key/value vectors so that
  `encode(parse(x)) == x` for spec-conforming input. Every parsed value
  records the byte range it occupied in the input (`rawBegin`/`rawEnd`).
  Strict validation: leading zeros, negative zero, trailing data,
  truncated strings and unbounded nesting are all rejected.
- **`TorrentFile`**: validates the metainfo structure and extracts the
  announce URL(s), piece length, piece hashes, and file list with global
  byte offsets. The **info hash** is SHA-1 over the *original raw bytes*
  of the `info` value (using the recorded spans) — hashing a re-encoded
  dictionary would produce a wrong hash for any non-canonical torrent.
  Path components are validated (`..`, separators) so a hostile torrent
  cannot escape the output directory.

### `tracker` — peer discovery
`TrackerClient` builds the announce URL (binary `info_hash`/`peer_id` are
percent-encoded), performs the HTTP(S) GET via **libcurl** with timeouts,
and parses the bencoded response: failure reasons, the announce interval,
and both peer-list formats (compact BEP 23 strings and dictionary lists).
`announce-list` tiers (BEP 12) are walked in order; the first responding
tracker wins. UDP trackers are skipped by design — the project speaks the
HTTP tracker protocol.

### `protocol` — the wire format
Pure data-transformation code, kept free of scheduling logic so it is
trivially unit-testable:
- `Handshake`: the fixed 68-byte handshake; parsing validates the
  protocol string, the caller validates the returned info hash.
- `Bitfield`: MSB-first piece bitmap with strict length/spare-bit checks.
- `message`: length-prefixed frame reader/writer (keep-alive = length 0)
  plus encoders/decoders for `request`, `piece`, and `have`. Frames above
  1 MiB are rejected as protocol violations.

### `peer` — connection lifecycle
- **`PeerConnection`** drives one remote peer synchronously: connect with
  timeout → handshake → verify info hash → `interested` → pipelined
  block requests (16 outstanding) whenever unchoked. Receive timeouts
  distinguish *idle* (tolerated for a few rounds, keep-alives sent) from
  *stalled with outstanding requests* (peer dropped). On any exit its
  claimed blocks are returned to the pool (RAII destructor).
- **`PeerManager`** owns a worker-thread pool (default 30). Workers pull
  addresses from a deduplicated queue; failed peers get one retry. The
  tracker layer feeds new addresses in as re-announces happen.

### `download` — scheduling and persistence
- **`PieceManager`** is the single source of truth for progress, shared
  by all peer threads behind one mutex. Pieces are split into 16 KiB
  blocks; `nextRequest()` prefers finishing in-flight pieces, then opens
  new ones, and re-assigns blocks whose request has been outstanding
  longer than 20 s (stalled-peer recovery). A completed piece is SHA-1
  hashed; on match it is written to storage, on mismatch it is reset and
  re-downloaded (the failure is counted and logged).
- **`FileStorage`** maps the torrent's contiguous byte stream onto the
  actual files (a piece may span several files), creating and
  pre-sizing them on open. A read-only mode supports `verify` without
  touching the disk layout.
- **`DownloadEngine`** wires everything: open storage → hash existing
  data (resume) → announce `started` → start workers → progress/ETA loop
  → re-announce on interval or when starved of peers → final
  `completed`/`stopped` announce.

### `crypto`, `util`
SHA-1 via OpenSSL's EVP interface (the non-deprecated 3.x API); a typed
exception hierarchy rooted at `tx::Error`; a thread-safe leveled logger;
an RAII TCP socket (POSIX + Winsock) with connect/receive timeouts; hex,
URL-encoding and big-endian helpers.

## Threading model

```
main thread          engine loop: progress display, re-announce timer
N worker threads     one PeerConnection each (blocking I/O)
```

Shared state is confined to three places, each with a single mutex:
`PieceManager` (block/piece state), `FileStorage` (stream access), and
`PeerManager`'s address queue (plus a condition variable). Peer threads
never share sockets or buffers; a piece buffer is owned by `PieceManager`
and filled under its lock. Lock ordering is acyclic: `PieceManager` →
`FileStorage`, never the reverse; no lock is held while blocking on the
network. Stop is signalled through `std::atomic<bool>` flags that workers
poll between (bounded) blocking operations.

## Error-handling strategy

All failures throw types from `util/error.hpp` (`BencodeError`,
`TorrentError`, `TrackerError`, `NetworkError`, `TimeoutError`,
`ProtocolError`, `StorageError`). Peer-level errors are caught in the
worker loop — a bad peer costs one log line and a retry, never the
download. Tracker errors fall through tiers before surfacing. Only the
CLI catches at top level and converts to exit codes; `TimeoutError` is
special-cased inside the peer loop to distinguish silence from failure.

## Design decisions and trade-offs

- **Thread-per-peer with blocking sockets** instead of an event loop:
  with ≤ 30 connections this is dramatically simpler to reason about and
  performs identically; an epoll/IOCP reactor would be justified only at
  hundreds of connections.
- **First-missing piece selection** instead of rarest-first: correct and
  simple; rarest-first matters for swarm health at scale and would slot
  into `PieceManager::nextRequest()` without touching other modules.
- **Timeout-based block stealing** instead of a full endgame mode: the
  last blocks of a download can be re-requested from another peer after
  20 s, which bounds tail latency without duplicate-request bookkeeping.
- **Download-only client**: TorrentX never unchokes remote peers or
  serves blocks. Upload support would extend `PeerConnection` with a
  request queue fed from `FileStorage` reads.
- **Ordered-vector bencode dictionaries**: preserves original key order
  (needed for byte-faithful round-trips), keeps `std::map`'s
  incomplete-type problems out of the recursive variant, and lookup cost
  is irrelevant at torrent-dictionary sizes.

## Testing

`tests/test_main.cpp` is a dependency-free test binary registered with
CTest. Beyond per-module unit tests it contains a full no-network
integration test: a synthetic torrent is built in memory, blocks are fed
through `PieceManager` exactly as a peer thread would, and the test then
re-verifies the written files from disk — including detecting a
deliberately corrupted byte. Live behaviour (tracker announce, handshake,
multi-peer download, resume) was validated against the Debian netinst
torrent's real swarm.
