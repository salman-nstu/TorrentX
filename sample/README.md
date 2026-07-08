# Sample torrent

`hello.torrent` describes `hello.txt` (single file, one 32 KiB piece,
info hash `d674c73735b120854ab266648b37daf2ebcec2a2`). Because the payload
sits right next to it, the metadata and verification commands work
offline:

```sh
TorrentX info   sample/hello.torrent       # parse and print metadata
TorrentX verify sample/hello.torrent -o sample   # 1/1 pieces valid
```

For a real download test, any well-seeded torrent with an HTTP tracker
works — for example the Debian installer images:

```sh
curl -LO https://cdimage.debian.org/debian-cd/current/amd64/bt-cd/debian-13.5.0-amd64-netinst.iso.torrent
TorrentX download debian-13.5.0-amd64-netinst.iso.torrent -o downloads
```
