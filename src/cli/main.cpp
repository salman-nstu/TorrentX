#include "commands.hpp"

#include "torrentx/util/error.hpp"
#include "torrentx/util/logger.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char kVersion[] = "1.0.0";

// Written from the SIGINT handler, polled by the download loop.
std::atomic<bool> g_stopRequested{false};

void onSignal(int)
{
    g_stopRequested = true;
}

void printUsage()
{
    std::printf(
        "TorrentX %s - BitTorrent client\n"
        "\n"
        "Usage:\n"
        "  TorrentX <command> <file.torrent> [options]\n"
        "\n"
        "Commands:\n"
        "  download   Download the torrent's payload\n"
        "  info       Show metadata from the .torrent file\n"
        "  peers      Announce to the tracker and list returned peers\n"
        "  verify     Check downloaded data against the piece hashes\n"
        "\n"
        "Options:\n"
        "  -o, --output <dir>     Output directory (default: current directory)\n"
        "      --max-peers <n>    Maximum simultaneous peer connections (default: 30)\n"
        "      --port <n>         Port reported to the tracker (default: 6881)\n"
        "  -v, --verbose          Enable debug logging\n"
        "  -h, --help             Show this help\n"
        "      --version          Show version\n"
        "\n"
        "Examples:\n"
        "  TorrentX info ubuntu.torrent\n"
        "  TorrentX download ubuntu.torrent -o ~/Downloads\n"
        "  TorrentX verify ubuntu.torrent -o ~/Downloads\n",
        kVersion);
}

/// Parses argv into command + options. Returns false on bad usage.
bool parseArguments(int argc, char** argv, std::string& command, tx::cli::Options& options)
{
    if (argc < 2) {
        return false;
    }
    command = argv[1];

    if (command == "-h" || command == "--help" || command == "help") {
        command = "help";
        return true;
    }
    if (command == "--version") {
        command = "version";
        return true;
    }

    int positional = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", arg.c_str());
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "-o" || arg == "--output") {
            const char* value = next();
            if (!value) return false;
            options.outputDir = value;
        } else if (arg == "--max-peers") {
            const char* value = next();
            if (!value) return false;
            const long parsed = std::strtol(value, nullptr, 10);
            if (parsed < 1 || parsed > 200) {
                std::fprintf(stderr, "error: --max-peers must be between 1 and 200\n");
                return false;
            }
            options.maxPeers = static_cast<size_t>(parsed);
        } else if (arg == "--port") {
            const char* value = next();
            if (!value) return false;
            const long parsed = std::strtol(value, nullptr, 10);
            if (parsed < 1 || parsed > 65535) {
                std::fprintf(stderr, "error: --port must be between 1 and 65535\n");
                return false;
            }
            options.port = static_cast<uint16_t>(parsed);
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            return false;
        } else if (positional == 0) {
            options.torrentPath = arg;
            ++positional;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
            return false;
        }
    }

    if (options.torrentPath.empty()) {
        std::fprintf(stderr, "error: no .torrent file given\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string command;
    tx::cli::Options options;

    if (!parseArguments(argc, argv, command, options)) {
        printUsage();
        return 2;
    }
    if (command == "help") {
        printUsage();
        return 0;
    }
    if (command == "version") {
        std::printf("TorrentX %s\n", kVersion);
        return 0;
    }

    if (options.verbose) {
        tx::log::Logger::instance().setLevel(tx::log::Level::Debug);
    }

    std::signal(SIGINT, onSignal);
#ifdef SIGTERM
    std::signal(SIGTERM, onSignal);
#endif

    try {
        if (command == "info") {
            return tx::cli::cmdInfo(options);
        }
        if (command == "peers") {
            return tx::cli::cmdPeers(options);
        }
        if (command == "download") {
            return tx::cli::cmdDownload(options, g_stopRequested);
        }
        if (command == "verify") {
            return tx::cli::cmdVerify(options);
        }
        std::fprintf(stderr, "error: unknown command '%s'\n\n", command.c_str());
        printUsage();
        return 2;
    } catch (const tx::Error& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected error: %s\n", e.what());
        return 1;
    }
}
