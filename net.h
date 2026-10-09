// net.h - the little bit of HTTP the UPnP side needs: a blocking client with
// timeouts (for SOAP calls and device descriptions) and a small threaded
// server (for handing track files to a renderer).  POSIX sockets only.
//
// Nothing here knows about UPnP; see upnp.h and track_server.h for that.

#ifndef PYJUKE_NET_H
#define PYJUKE_NET_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pyjuke::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

// Case-insensitive lookup; empty string if absent.
std::string header_value(const Headers& headers, std::string_view name);

struct Url {
    std::string scheme;  // "http"
    std::string host;    // name or address literal (IPv6 without brackets)
    int port = 80;
    std::string path;    // starts with '/', includes any query
};

// http://host[:port][/path]. Only http is accepted.
std::optional<Url> parse_url(std::string_view text);

// Resolve `ref` against `base`: absolute URLs pass through, "/x" keeps the
// authority, anything else is relative to base's directory.
std::string resolve_url(const std::string& base, const std::string& ref);

struct HttpRequest {
    std::string method;   // "GET", "HEAD", "POST", ...
    std::string path;     // as sent, e.g. "/track/12.mp3"
    Headers headers;
    std::string body;
};

struct HttpResponse {
    int status = 0;
    std::string reason;
    Headers headers;
    std::string body;

    // Server side only: if `file` is set the server streams
    // file[file_offset, file_offset + file_length) as the body instead.
    std::string file;
    uint64_t file_offset = 0;
    uint64_t file_length = 0;

    std::string header(std::string_view name) const { return header_value(headers, name); }
};

// Parse "status line + headers" (up to and including the blank line).
// Returns the number of bytes consumed, or 0 if the head is incomplete/bad.
size_t parse_response_head(std::string_view data, HttpResponse& out);
size_t parse_request_head(std::string_view data, HttpRequest& out);

// Decode a chunked transfer-encoded body. False if malformed.
bool decode_chunked(std::string_view in, std::string& out);

// One request with "Connection: close". Returns nullopt and sets *error on a
// transport failure (connect, timeout, malformed reply); HTTP error statuses
// are returned normally.
std::optional<HttpResponse> http_request(const std::string& method, const Url& url,
                                         const Headers& headers, const std::string& body,
                                         int timeout_ms, std::string* error = nullptr);

// Address of the local interface that routes to `remote_host`, as a string
// ("192.168.1.20"), or empty on failure.
std::string local_address_for(const std::string& remote_host);

// A minimal HTTP/1.1 server: one accept thread, one thread per connection,
// every connection closed after its response. The handler is called on a
// connection thread and must be thread-safe.
class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer();
    ~HttpServer();  // stop()s
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Listen on 0.0.0.0:port (0 = any free port). False and *error on failure.
    bool start(uint16_t port, Handler handler, std::string* error = nullptr);
    void stop();
    bool running() const;
    uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pyjuke::net

#endif
