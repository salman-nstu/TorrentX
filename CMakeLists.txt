cmake_minimum_required(VERSION 3.16)

project(TorrentX
    VERSION 1.0.0
    DESCRIPTION "Peer-to-peer file distribution engine (BitTorrent client)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Core engine as a static library so the CLI and the tests share one build.
add_library(torrentx_core STATIC
    src/crypto/sha1.cpp
    src/download/download_engine.cpp
    src/download/file_storage.cpp
    src/download/piece_manager.cpp
    src/peer/peer_connection.cpp
    src/peer/peer_manager.cpp
    src/protocol/handshake.cpp
    src/protocol/message.cpp
    src/torrent/bencode.cpp
    src/torrent/torrent_file.cpp
    src/tracker/tracker_client.cpp
    src/util/byte_utils.cpp
    src/util/logger.cpp
    src/util/tcp_socket.cpp
)

target_include_directories(torrentx_core PUBLIC include)

target_link_libraries(torrentx_core
    PUBLIC
    CURL::libcurl
    OpenSSL::Crypto
    Threads::Threads
)

if(WIN32)
    target_link_libraries(torrentx_core PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(torrentx_core PRIVATE /W4)
else()
    target_compile_options(torrentx_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(TorrentX
    src/cli/main.cpp
    src/cli/commands.cpp
)

target_link_libraries(TorrentX PRIVATE torrentx_core)

if(MSVC)
    target_compile_options(TorrentX PRIVATE /W4)
else()
    target_compile_options(TorrentX PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_subdirectory(tests)
