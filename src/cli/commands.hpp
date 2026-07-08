#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace tx::cli {

/// Options shared by all commands, filled by the argument parser.
struct Options {
    std::string torrentPath;
    std::string outputDir = ".";
    size_t maxPeers = 30;
    uint16_t port = 6881;
    bool verbose = false;
};

/// Each command returns a process exit code.
int cmdInfo(const Options& options);
int cmdPeers(const Options& options);
int cmdDownload(const Options& options, const std::atomic<bool>& stopRequested);
int cmdVerify(const Options& options);

} // namespace tx::cli
