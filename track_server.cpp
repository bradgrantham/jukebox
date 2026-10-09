// track_server.cpp - see track_server.h.

#include "track_server.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace fs = std::filesystem;

namespace pyjuke {

namespace {

// "/track/<n>.mp3" -> n
std::optional<size_t> parse_track_path(std::string_view path)
{
    constexpr std::string_view prefix = "/track/";
    constexpr std::string_view suffix = ".mp3";
    size_t q = path.find('?');
    if (q != std::string_view::npos) path = path.substr(0, q);
    if (path.substr(0, prefix.size()) != prefix) return std::nullopt;
    path.remove_prefix(prefix.size());
    if (path.size() <= suffix.size() || path.substr(path.size() - suffix.size()) != suffix) return std::nullopt;
    path.remove_suffix(suffix.size());
    if (path.empty() || path.size() > 12) return std::nullopt;
    size_t n = 0;
    for (char c : path) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        n = n * 10 + static_cast<size_t>(c - '0');
    }
    return n;
}

// "bytes=a-b", "bytes=a-", "bytes=-n" against `size`. Returns false if the
// header is present but unsatisfiable or malformed.
bool parse_range(std::string_view header, uint64_t size, uint64_t* first, uint64_t* last)
{
    constexpr std::string_view unit = "bytes=";
    if (header.substr(0, unit.size()) != unit) return false;
    header.remove_prefix(unit.size());
    if (header.find(',') != std::string_view::npos) return false;  // multiple ranges: not supported
    size_t dash = header.find('-');
    if (dash == std::string_view::npos) return false;
    std::string_view a = header.substr(0, dash), b = header.substr(dash + 1);
    auto number = [](std::string_view s, uint64_t* v) {
        if (s.empty() || s.size() > 18) return false;
        uint64_t n = 0;
        for (char c : s) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            n = n * 10 + static_cast<uint64_t>(c - '0');
        }
        *v = n;
        return true;
    };
    if (size == 0) return false;
    if (a.empty()) {
        uint64_t n;
        if (!number(b, &n) || n == 0) return false;
        n = std::min(n, size);
        *first = size - n;
        *last = size - 1;
        return true;
    }
    if (!number(a, first)) return false;
    if (*first >= size) return false;
    if (b.empty()) *last = size - 1;
    else if (!number(b, last)) return false;
    if (*last < *first) return false;
    *last = std::min(*last, size - 1);
    return true;
}

} // namespace

TrackServer::TrackServer(std::vector<fs::path> tracks) : tracks_(std::move(tracks)) {}

TrackServer::~TrackServer()
{
    stop();
}

bool TrackServer::start(uint16_t port, std::string* error)
{
    return server_.start(port, [this](const net::HttpRequest& r) { return handle(r); }, error);
}

void TrackServer::stop()
{
    server_.stop();
}

std::optional<size_t> TrackServer::index_of(const fs::path& track) const
{
    auto it = std::lower_bound(tracks_.begin(), tracks_.end(), track,
                               [](const fs::path& a, const fs::path& b) { return a.native() < b.native(); });
    if (it != tracks_.end() && *it == track) return static_cast<size_t>(it - tracks_.begin());
    // Not sorted the way we expect (or not found): fall back to a scan.
    auto f = std::find(tracks_.begin(), tracks_.end(), track);
    if (f != tracks_.end()) return static_cast<size_t>(f - tracks_.begin());
    return std::nullopt;
}

std::string TrackServer::url_for(size_t index, const std::string& host) const
{
    std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    return "http://" + h + ":" + std::to_string(port()) + "/track/" + std::to_string(index) + ".mp3";
}

std::optional<size_t> TrackServer::index_from_url(const std::string& url)
{
    auto u = net::parse_url(url);
    if (!u) return std::nullopt;
    return parse_track_path(u->path);
}

net::HttpResponse TrackServer::handle(const net::HttpRequest& request) const
{
    net::HttpResponse resp;
    auto plain = [&](int status, const char* text) {
        resp.status = status;
        resp.body = text;
        return resp;
    };
    if (request.method != "GET" && request.method != "HEAD") {
        resp.headers.emplace_back("Allow", "GET, HEAD");
        return plain(405, "method not allowed\n");
    }
    auto index = parse_track_path(request.path);
    if (!index || *index >= tracks_.size()) return plain(404, "not found\n");

    const std::string path = tracks_[*index].string();
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return plain(404, "not found\n");
    const uint64_t size = static_cast<uint64_t>(st.st_size);

    resp.headers.emplace_back("Content-Type", "audio/mpeg");
    resp.headers.emplace_back("Accept-Ranges", "bytes");
    resp.headers.emplace_back("Cache-Control", "no-cache");
    // What a DLNA renderer asks about with getcontentFeatures.dlna.org.
    resp.headers.emplace_back("contentFeatures.dlna.org",
                              "DLNA.ORG_PN=MP3;DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000");
    resp.headers.emplace_back("transferMode.dlna.org", "Streaming");

    uint64_t first = 0, last = size == 0 ? 0 : size - 1;
    std::string range = net::header_value(request.headers, "Range");
    if (!range.empty()) {
        if (!parse_range(range, size, &first, &last)) {
            resp.headers.emplace_back("Content-Range", "bytes */" + std::to_string(size));
            return plain(416, "range not satisfiable\n");
        }
        resp.status = 206;
        resp.headers.emplace_back("Content-Range", "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
                                                       std::to_string(size));
    } else {
        resp.status = 200;
    }
    resp.file = path;
    resp.file_offset = first;
    resp.file_length = size == 0 ? 0 : last - first + 1;
    return resp;
}

} // namespace pyjuke
