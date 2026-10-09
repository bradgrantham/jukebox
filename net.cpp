// net.cpp - see net.h.

#include "net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <list>
#include <mutex>
#include <thread>

namespace pyjuke::net {

namespace {

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Parses "Name: value" lines starting at `pos` up to the blank line. Returns
// the position just past the blank line, or npos if there is none.
size_t parse_header_lines(std::string_view data, size_t pos, Headers& out)
{
    for (;;) {
        size_t eol = data.find("\r\n", pos);
        if (eol == std::string_view::npos) return std::string_view::npos;
        if (eol == pos) return eol + 2;  // blank line
        std::string_view line = data.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string_view::npos)
            out.emplace_back(std::string(trim(line.substr(0, colon))), std::string(trim(line.substr(colon + 1))));
        pos = eol + 2;
    }
}

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int fd() const { return fd_; }
    int release() { int f = fd_; fd_ = -1; return f; }
    void close()
    {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
private:
    int fd_ = -1;
};

void set_timeouts(int fd, int timeout_ms)
{
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool send_all(int fd, const char* data, size_t len)
{
    while (len > 0) {
        ssize_t n = ::send(fd, data, len, kSendFlags);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// Connect with a timeout; returns the fd or -1 (errno/message in *error).
int connect_with_timeout(const std::string& host, int port, int timeout_ms, std::string* error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    if (int r = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res); r != 0) {
        if (error) *error = "cannot resolve " + host + ": " + gai_strerror(r);
        return -1;
    }
    int fd = -1;
    std::string last = "no addresses";
    for (addrinfo* ai = res; ai != nullptr && fd < 0; ai = ai->ai_next) {
        Socket s(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (s.fd() < 0) {
            last = std::strerror(errno);
            continue;
        }
        int flags = fcntl(s.fd(), F_GETFL, 0);
        fcntl(s.fd(), F_SETFL, flags | O_NONBLOCK);
        int r = ::connect(s.fd(), ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && errno != EINPROGRESS) {
            last = std::strerror(errno);
            continue;
        }
        if (r < 0) {
            pollfd p{s.fd(), POLLOUT, 0};
            int pr = ::poll(&p, 1, timeout_ms);
            if (pr <= 0) {
                last = pr == 0 ? "connect timed out" : std::strerror(errno);
                continue;
            }
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(s.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) {
                last = std::strerror(err);
                continue;
            }
        }
        fcntl(s.fd(), F_SETFL, flags);
        set_timeouts(s.fd(), timeout_ms);
        int one = 1;
        setsockopt(s.fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fd = s.release();
    }
    freeaddrinfo(res);
    if (fd < 0 && error) *error = "connect to " + host + ":" + port_str + " failed: " + last;
    return fd;
}

// Read until the predicate on the accumulated buffer says we're done, a
// limit is hit, or the peer closes. Returns false on a read error/timeout.
template <class Done>
bool read_until(int fd, std::string& buf, size_t limit, Done done, bool* closed)
{
    *closed = false;
    char chunk[8192];
    while (!done(buf) && buf.size() < limit) {
        ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            *closed = true;
            return true;
        }
        buf.append(chunk, static_cast<size_t>(n));
    }
    return true;
}

constexpr size_t kMaxHead = 64 * 1024;
constexpr size_t kMaxBody = 4 * 1024 * 1024;

std::string status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    default: return "Unknown";
    }
}

} // namespace

std::string header_value(const Headers& headers, std::string_view name)
{
    for (const auto& [k, v] : headers)
        if (iequals(k, name)) return v;
    return {};
}

std::optional<Url> parse_url(std::string_view text)
{
    text = trim(text);
    size_t scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos) return std::nullopt;
    Url u;
    u.scheme = std::string(text.substr(0, scheme_end));
    std::transform(u.scheme.begin(), u.scheme.end(), u.scheme.begin(), [](unsigned char c) { return std::tolower(c); });
    if (u.scheme != "http") return std::nullopt;
    std::string_view rest = text.substr(scheme_end + 3);
    size_t slash = rest.find('/');
    std::string_view authority = rest.substr(0, slash);
    u.path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    if (authority.empty()) return std::nullopt;
    std::string_view host, port;
    if (authority.front() == '[') {
        size_t close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host = authority.substr(1, close - 1);
        std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return std::nullopt;
            port = after.substr(1);
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            host = authority;
        } else {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
    }
    if (host.empty()) return std::nullopt;
    u.host = std::string(host);
    if (!port.empty()) {
        int p = 0;
        for (char c : port) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
            p = p * 10 + (c - '0');
            if (p > 65535) return std::nullopt;
        }
        if (p == 0) return std::nullopt;
        u.port = p;
    }
    return u;
}

std::string resolve_url(const std::string& base, const std::string& ref)
{
    if (ref.find("://") != std::string::npos) return ref;
    auto b = parse_url(base);
    if (!b) return ref;
    std::string origin = b->scheme + "://" + (b->host.find(':') != std::string::npos ? "[" + b->host + "]" : b->host);
    if (b->port != 80) origin += ":" + std::to_string(b->port);
    if (ref.empty()) return origin + b->path;
    if (ref.front() == '/') return origin + ref;
    std::string dir = b->path;
    size_t q = dir.find('?');
    if (q != std::string::npos) dir.resize(q);
    size_t last = dir.rfind('/');
    dir = last == std::string::npos ? "/" : dir.substr(0, last + 1);
    return origin + dir + ref;
}

size_t parse_response_head(std::string_view data, HttpResponse& out)
{
    size_t eol = data.find("\r\n");
    if (eol == std::string_view::npos) return 0;
    std::string_view line = data.substr(0, eol);
    if (line.substr(0, 5) != "HTTP/") return 0;
    size_t sp = line.find(' ');
    if (sp == std::string_view::npos || sp + 4 > line.size()) return 0;
    int status = 0;
    for (size_t i = sp + 1; i < sp + 4; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(line[i]))) return 0;
        status = status * 10 + (line[i] - '0');
    }
    out.status = status;
    out.reason = sp + 4 < line.size() ? std::string(trim(line.substr(sp + 4))) : "";
    out.headers.clear();
    size_t end = parse_header_lines(data, eol + 2, out.headers);
    return end == std::string_view::npos ? 0 : end;
}

size_t parse_request_head(std::string_view data, HttpRequest& out)
{
    size_t eol = data.find("\r\n");
    if (eol == std::string_view::npos) return 0;
    std::string_view line = data.substr(0, eol);
    size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return 0;
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return 0;
    if (line.substr(sp2 + 1, 5) != "HTTP/") return 0;
    out.method = std::string(line.substr(0, sp1));
    out.path = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
    if (out.method.empty() || out.path.empty()) return 0;
    out.headers.clear();
    size_t end = parse_header_lines(data, eol + 2, out.headers);
    return end == std::string_view::npos ? 0 : end;
}

bool decode_chunked(std::string_view in, std::string& out)
{
    out.clear();
    size_t pos = 0;
    for (;;) {
        size_t eol = in.find("\r\n", pos);
        if (eol == std::string_view::npos) return false;
        std::string_view size_field = in.substr(pos, eol - pos);
        size_t semi = size_field.find(';');
        if (semi != std::string_view::npos) size_field = size_field.substr(0, semi);
        size_field = trim(size_field);
        if (size_field.empty()) return false;
        size_t size = 0;
        for (char c : size_field) {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            if (size > (kMaxBody >> 4)) return false;
            size = size * 16 + static_cast<size_t>(d);
        }
        pos = eol + 2;
        if (size == 0) return true;  // trailers, if any, are ignored
        if (pos + size + 2 > in.size()) return false;
        out.append(in.data() + pos, size);
        pos += size;
        if (in.substr(pos, 2) != "\r\n") return false;
        pos += 2;
    }
}

std::optional<HttpResponse> http_request(const std::string& method, const Url& url, const Headers& headers,
                                         const std::string& body, int timeout_ms, std::string* error)
{
    Socket s(connect_with_timeout(url.host, url.port, timeout_ms, error));
    if (s.fd() < 0) return std::nullopt;

    std::string host_header = url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host;
    if (url.port != 80) host_header += ":" + std::to_string(url.port);
    std::string req = method + " " + url.path + " HTTP/1.1\r\nHost: " + host_header + "\r\nConnection: close\r\nUser-Agent: pyjuke/1.0 UPnP/1.0\r\n";
    bool has_length = false;
    for (const auto& [k, v] : headers) {
        req += k + ": " + v + "\r\n";
        if (iequals(k, "Content-Length")) has_length = true;
    }
    if (!has_length && (!body.empty() || method == "POST")) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    req += body;
    if (!send_all(s.fd(), req.data(), req.size())) {
        if (error) *error = std::string("send failed: ") + std::strerror(errno);
        return std::nullopt;
    }

    std::string buf;
    bool closed = false;
    auto have_head = [](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; };
    if (!read_until(s.fd(), buf, kMaxHead, have_head, &closed)) {
        if (error) *error = std::string("read failed: ") + std::strerror(errno);
        return std::nullopt;
    }
    HttpResponse resp;
    size_t head = parse_response_head(buf, resp);
    if (head == 0) {
        if (error) *error = "malformed HTTP response";
        return std::nullopt;
    }
    std::string rest = buf.substr(head);
    bool chunked = iequals(trim(resp.header("Transfer-Encoding")), "chunked");
    std::string length_text = resp.header("Content-Length");
    if (method == "HEAD" || resp.status == 204 || resp.status == 304) {
        resp.body.clear();
    } else if (chunked) {
        auto complete = [](const std::string& b) {
            std::string tmp;
            return decode_chunked(b, tmp);
        };
        if (!closed && !read_until(s.fd(), rest, kMaxBody, complete, &closed)) {
            if (error) *error = std::string("read failed: ") + std::strerror(errno);
            return std::nullopt;
        }
        if (!decode_chunked(rest, resp.body)) {
            if (error) *error = "malformed chunked body";
            return std::nullopt;
        }
    } else if (!length_text.empty()) {
        size_t want = static_cast<size_t>(std::strtoull(length_text.c_str(), nullptr, 10));
        if (want > kMaxBody) {
            if (error) *error = "response too large";
            return std::nullopt;
        }
        auto complete = [want](const std::string& b) { return b.size() >= want; };
        if (!closed && !read_until(s.fd(), rest, want, complete, &closed)) {
            if (error) *error = std::string("read failed: ") + std::strerror(errno);
            return std::nullopt;
        }
        if (rest.size() < want) {
            if (error) *error = "response truncated";
            return std::nullopt;
        }
        resp.body = rest.substr(0, want);
    } else {
        // Delimited by close.
        auto never = [](const std::string&) { return false; };
        if (!closed && !read_until(s.fd(), rest, kMaxBody, never, &closed)) {
            if (error) *error = std::string("read failed: ") + std::strerror(errno);
            return std::nullopt;
        }
        resp.body = std::move(rest);
    }
    return resp;
}

std::string local_address_for(const std::string& remote_host)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(remote_host.c_str(), "9", &hints, &res) != 0) return {};
    std::string result;
    for (addrinfo* ai = res; ai != nullptr && result.empty(); ai = ai->ai_next) {
        Socket s(::socket(ai->ai_family, ai->ai_socktype, 0));
        if (s.fd() < 0) continue;
        if (::connect(s.fd(), ai->ai_addr, ai->ai_addrlen) != 0) continue;
        sockaddr_storage local{};
        socklen_t len = sizeof local;
        if (getsockname(s.fd(), reinterpret_cast<sockaddr*>(&local), &len) != 0) continue;
        char buf[INET6_ADDRSTRLEN] = {};
        if (local.ss_family == AF_INET)
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&local)->sin_addr, buf, sizeof buf);
        else if (local.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&local)->sin6_addr, buf, sizeof buf);
        result = buf;
    }
    freeaddrinfo(res);
    return result;
}

// ---------------------------------------------------------------------------
// HttpServer

struct HttpServer::Impl {
    Handler handler;
    int listen_fd = -1;
    uint16_t port = 0;
    std::atomic<bool> stopping{false};
    std::thread accept_thread;

    struct Connection {
        int fd;
        std::thread thread;
        bool finished = false;
    };
    std::mutex mutex;
    std::list<Connection> connections;

    void accept_loop();
    void serve(int fd);
    void reap_finished();
};

HttpServer::HttpServer() : impl_(std::make_unique<Impl>()) {}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::start(uint16_t port, Handler handler, std::string* error)
{
    stop();
    impl_->stopping = false;
    impl_->handler = std::move(handler);
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 16) != 0) {
        if (error) *error = std::string("bind/listen on port ") + std::to_string(port) + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    socklen_t len = sizeof addr;
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    impl_->port = ntohs(addr.sin_port);
    impl_->listen_fd = fd;
    impl_->accept_thread = std::thread([this] { impl_->accept_loop(); });
    return true;
}

void HttpServer::stop()
{
    Impl& im = *impl_;
    if (im.listen_fd < 0) return;
    im.stopping = true;
    ::shutdown(im.listen_fd, SHUT_RDWR);
    ::close(im.listen_fd);
    im.listen_fd = -1;
    if (im.accept_thread.joinable()) im.accept_thread.join();
    {
        std::lock_guard<std::mutex> lock(im.mutex);
        for (auto& c : im.connections)
            if (c.fd >= 0) ::shutdown(c.fd, SHUT_RDWR);
    }
    // Only this thread touches the list now; a connection thread's last act
    // is to mark itself finished, so its node must outlive the join.
    for (;;) {
        std::unique_lock<std::mutex> lock(im.mutex);
        if (im.connections.empty()) break;
        auto it = im.connections.begin();
        std::thread t = std::move(it->thread);
        lock.unlock();
        if (t.joinable()) t.join();
        lock.lock();
        im.connections.erase(it);
    }
}

bool HttpServer::running() const
{
    return impl_->listen_fd >= 0;
}

uint16_t HttpServer::port() const
{
    return impl_->port;
}

void HttpServer::Impl::reap_finished()
{
    // Called with mutex held.
    for (auto it = connections.begin(); it != connections.end();) {
        if (it->finished) {
            it->thread.join();
            it = connections.erase(it);
        } else {
            ++it;
        }
    }
}

void HttpServer::Impl::accept_loop()
{
    while (!stopping) {
        pollfd p{listen_fd, POLLIN, 0};
        int r = ::poll(&p, 1, 200);
        if (r <= 0) continue;
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        set_timeouts(fd, 15000);
        std::lock_guard<std::mutex> lock(mutex);
        reap_finished();
        connections.push_back({fd, {}, false});
        Connection* conn = &connections.back();
        conn->thread = std::thread([this, conn] {
            serve(conn->fd);
            std::lock_guard<std::mutex> l(mutex);
            ::close(conn->fd);
            conn->fd = -1;
            conn->finished = true;
        });
    }
}

void HttpServer::Impl::serve(int fd)
{
    std::string buf;
    bool closed = false;
    auto have_head = [](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; };
    if (!read_until(fd, buf, kMaxHead, have_head, &closed)) return;
    HttpRequest req;
    size_t head = parse_request_head(buf, req);
    HttpResponse resp;
    if (head == 0) {
        resp.status = 400;
        resp.body = "bad request\n";
    } else {
        std::string length_text = header_value(req.headers, "Content-Length");
        size_t want = length_text.empty() ? 0 : static_cast<size_t>(std::strtoull(length_text.c_str(), nullptr, 10));
        req.body = buf.substr(head);
        if (want > kMaxBody) {
            resp.status = 400;
            resp.body = "body too large\n";
        } else {
            auto complete = [want](const std::string& b) { return b.size() >= want; };
            if (!closed && !read_until(fd, req.body, want, complete, &closed)) return;
            req.body.resize(std::min(req.body.size(), want));
            resp = handler(req);
        }
    }

    std::ifstream file;
    uint64_t body_length = resp.body.size();
    if (!resp.file.empty()) {
        file.open(resp.file, std::ios::binary);
        if (!file) {
            resp = HttpResponse{};
            resp.status = 404;
            resp.body = "not found\n";
            body_length = resp.body.size();
        } else {
            file.seekg(static_cast<std::streamoff>(resp.file_offset));
            body_length = resp.file_length;
        }
    }
    if (resp.status == 0) resp.status = 500;
    if (resp.reason.empty()) resp.reason = status_text(resp.status);

    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + resp.reason + "\r\n";
    bool have_type = false;
    for (const auto& [k, v] : resp.headers) {
        if (iequals(k, "Content-Length") || iequals(k, "Connection")) continue;
        if (iequals(k, "Content-Type")) have_type = true;
        out += k + ": " + v + "\r\n";
    }
    if (!have_type) out += "Content-Type: text/plain; charset=utf-8\r\n";
    out += "Content-Length: " + std::to_string(body_length) + "\r\nConnection: close\r\n\r\n";
    if (!send_all(fd, out.data(), out.size())) return;
    if (req.method == "HEAD") return;

    if (file.is_open()) {
        std::vector<char> chunk(64 * 1024);
        uint64_t left = body_length;
        while (left > 0 && !stopping) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(left, chunk.size()));
            file.read(chunk.data(), static_cast<std::streamsize>(n));
            std::streamsize got = file.gcount();
            if (got <= 0) break;
            if (!send_all(fd, chunk.data(), static_cast<size_t>(got))) break;
            left -= static_cast<uint64_t>(got);
        }
    } else {
        send_all(fd, resp.body.data(), resp.body.size());
    }
}

} // namespace pyjuke::net
