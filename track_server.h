// track_server.h - serves the scanned library over HTTP so a UPnP renderer
// can fetch the track pyjuke tells it to play.
//
//   GET /track/<index>.mp3      the file at tracks[index], with byte ranges
//
// Only indexes into the fixed, sorted track list are reachable; no path from
// the network ever touches the filesystem.

#ifndef PYJUKE_TRACK_SERVER_H
#define PYJUKE_TRACK_SERVER_H

#include "net.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pyjuke {

class TrackServer {
public:
    // `tracks` must be sorted by path (scan_library's order), so that
    // index_of() can binary-search it.
    explicit TrackServer(std::vector<std::filesystem::path> tracks);
    ~TrackServer();

    // Listen on `port` (0 = any). False and *error on failure.
    bool start(uint16_t port, std::string* error = nullptr);
    void stop();
    bool running() const { return server_.running(); }
    uint16_t port() const { return server_.port(); }

    size_t track_count() const { return tracks_.size(); }
    std::optional<size_t> index_of(const std::filesystem::path& track) const;
    // "http://<host>:<port>/track/<index>.mp3"; `host` is the address the
    // renderer should connect to (see net::local_address_for).
    std::string url_for(size_t index, const std::string& host) const;
    // The index a URL made by url_for() names, whatever host it carries.
    static std::optional<size_t> index_from_url(const std::string& url);

    // The request handler, usable without a running server (tests).
    net::HttpResponse handle(const net::HttpRequest& request) const;

private:
    std::vector<std::filesystem::path> tracks_;
    net::HttpServer server_;
};

} // namespace pyjuke

#endif
