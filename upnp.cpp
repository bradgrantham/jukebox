// upnp.cpp - see upnp.h.

#include "upnp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace pyjuke::upnp {

const char* const kMp3ProtocolInfo =
    "http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000";

namespace {

int64_t steady_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Position of the first "<tag>" or "<tag ..." start tag, and the position
// just past its '>'. Returns npos if absent.
size_t find_start_tag(std::string_view xml, std::string_view tag, size_t from, size_t* content_begin)
{
    size_t pos = from;
    for (;;) {
        pos = xml.find('<', pos);
        if (pos == std::string_view::npos) return pos;
        std::string_view rest = xml.substr(pos + 1);
        if (rest.substr(0, tag.size()) == tag) {
            char after = rest.size() > tag.size() ? rest[tag.size()] : '\0';
            if (after == '>' || after == ' ' || after == '\t' || after == '\n' || after == '\r' || after == '/') {
                size_t close = xml.find('>', pos);
                if (close == std::string_view::npos) return close;
                if (content_begin) *content_begin = close + 1;
                return pos;
            }
        }
        ++pos;
    }
}

// Position of the first "</tag>" (or "</tag >") at or after `from`; npos if none.
size_t find_end_tag(std::string_view xml, std::string_view tag, size_t from)
{
    std::string end_tag = "</" + std::string(tag);
    for (size_t pos = from;;) {
        pos = xml.find(end_tag, pos);
        if (pos == std::string_view::npos) return pos;
        size_t after = pos + end_tag.size();
        if (after < xml.size() && (xml[after] == '>' || std::isspace(static_cast<unsigned char>(xml[after])))) return pos;
        pos = after;
    }
}

} // namespace

std::string_view xml_element_raw(std::string_view xml, std::string_view tag)
{
    size_t content = 0;
    size_t start = find_start_tag(xml, tag, 0, &content);
    if (start == std::string_view::npos) return {};
    if (content >= 2 && xml[content - 2] == '/') return xml.substr(start, content - start);  // <tag/>
    size_t end = find_end_tag(xml, tag, content);
    if (end == std::string_view::npos) return {};
    size_t close = xml.find('>', end);
    if (close == std::string_view::npos) return {};
    return xml.substr(start, close + 1 - start);
}

std::string xml_element(std::string_view xml, std::string_view tag)
{
    size_t content = 0;
    size_t start = find_start_tag(xml, tag, 0, &content);
    if (start == std::string_view::npos) return {};
    if (content >= 2 && xml[content - 2] == '/') return {};
    size_t end = find_end_tag(xml, tag, content);
    if (end == std::string_view::npos) return {};
    return xml_unescape(trim(xml.substr(content, end - content)));
}

std::string xml_escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string xml_unescape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        size_t semi = text.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out += '&';
            continue;
        }
        std::string_view ent = text.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            bool ok = ent.size() > 1;
            if (ok && (ent[1] == 'x' || ent[1] == 'X')) {
                for (size_t k = 2; k < ent.size() && ok; ++k) {
                    char c = ent[k];
                    if (std::isxdigit(static_cast<unsigned char>(c))) cp = cp * 16 + static_cast<unsigned long>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(c) - 'a' + 10));
                    else ok = false;
                }
                ok = ok && ent.size() > 2;
            } else {
                for (size_t k = 1; k < ent.size() && ok; ++k) {
                    if (std::isdigit(static_cast<unsigned char>(ent[k]))) cp = cp * 10 + static_cast<unsigned long>(ent[k] - '0');
                    else ok = false;
                }
            }
            if (!ok || cp > 0x10FFFF) {
                out += '&';
                continue;
            }
            // UTF-8 encode.
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else {
            out += '&';
            continue;
        }
        i = semi;
    }
    return out;
}

int64_t parse_upnp_time(std::string_view text)
{
    text = trim(text);
    if (text.empty() || text == "NOT_IMPLEMENTED") return -1;
    // Split on ':'; the last field may carry a fraction.
    std::vector<std::string_view> fields;
    size_t pos = 0;
    for (;;) {
        size_t colon = text.find(':', pos);
        fields.push_back(text.substr(pos, colon == std::string_view::npos ? std::string_view::npos : colon - pos));
        if (colon == std::string_view::npos) break;
        pos = colon + 1;
    }
    if (fields.empty() || fields.size() > 3) return -1;
    auto digits = [](std::string_view f, int64_t* v) {
        if (f.empty()) return false;
        int64_t n = 0;
        for (char c : f) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            n = n * 10 + (c - '0');
            if (n > 1'000'000'000) return false;
        }
        *v = n;
        return true;
    };
    int64_t secs = 0;  // hours and minutes fields
    for (size_t i = 0; i + 1 < fields.size(); ++i) {
        int64_t v;
        if (!digits(fields[i], &v)) return -1;
        secs = secs * 60 + v;
    }
    std::string_view last = fields.back();
    std::string_view frac;
    size_t dot = last.find('.');
    if (dot != std::string_view::npos) {
        frac = last.substr(dot + 1);
        last = last.substr(0, dot);
    }
    int64_t s;
    if (!digits(last, &s)) return -1;
    secs = secs * 60 + s;
    int64_t ms = secs * 1000;
    if (!frac.empty()) {
        size_t slash = frac.find('/');
        if (slash != std::string_view::npos) {
            int64_t num, den;
            if (!digits(frac.substr(0, slash), &num) || !digits(frac.substr(slash + 1), &den) || den == 0) return -1;
            ms += num * 1000 / den;
        } else {
            int64_t v;
            if (!digits(frac, &v)) return -1;
            size_t n = frac.size();
            while (n > 3) v /= 10, --n;
            while (n < 3) v *= 10, ++n;
            ms += v;
        }
    }
    return ms;
}

std::string format_upnp_time(int64_t ms)
{
    if (ms < 0) ms = 0;
    int64_t t = ms / 1000;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", static_cast<long long>(t / 3600),
                  static_cast<long long>((t / 60) % 60), static_cast<long long>(t % 60));
    return buf;
}

net::Headers parse_ssdp_headers(std::string_view datagram)
{
    net::Headers out;
    size_t pos = datagram.find("\r\n");
    if (pos == std::string_view::npos) return out;
    pos += 2;
    while (pos < datagram.size()) {
        size_t eol = datagram.find("\r\n", pos);
        std::string_view line = datagram.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon != std::string_view::npos)
            out.emplace_back(std::string(trim(line.substr(0, colon))), std::string(trim(line.substr(colon + 1))));
        if (eol == std::string_view::npos) break;
        pos = eol + 2;
    }
    return out;
}

bool parse_device_description(std::string_view xml, const std::string& location, Renderer& out)
{
    // Find the <service> whose serviceType is AVTransport (any version).
    std::string_view rest = xml;
    std::string_view service;
    for (;;) {
        std::string_view s = xml_element_raw(rest, "service");
        if (s.empty()) break;
        std::string type = xml_element(s, "serviceType");
        if (type.find(":service:AVTransport:") != std::string::npos) {
            service = s;
            break;
        }
        rest = rest.substr(static_cast<size_t>(s.data() - rest.data()) + s.size());
    }
    if (service.empty()) return false;
    std::string control = xml_element(service, "controlURL");
    if (control.empty()) return false;

    std::string base = xml_element(xml, "URLBase");
    if (base.empty()) base = location;
    out.location = location;
    out.control_url = net::resolve_url(base, control);
    if (auto u = net::parse_url(out.control_url)) out.host = u->host;
    else return false;

    // Name and UDN of the device owning that service: the innermost <device>
    // whose extent contains it (an embedded renderer inside another root).
    size_t service_offset = static_cast<size_t>(service.data() - xml.data());
    std::string name, udn;
    size_t pos = 0;
    for (;;) {
        size_t content = 0;
        size_t start = find_start_tag(xml, "device", pos, &content);
        if (start == std::string_view::npos) break;
        std::string_view dev = xml_element_raw(xml.substr(start), "device");
        if (dev.empty()) break;
        if (service_offset >= start && service_offset < start + dev.size()) {
            std::string n = xml_element(dev, "friendlyName");
            std::string u = xml_element(dev, "UDN");
            if (!n.empty()) name = n;
            if (!u.empty()) udn = u;
        }
        pos = content;  // keep scanning, including nested devices
    }
    if (name.empty()) name = xml_element(xml, "friendlyName");
    if (udn.empty()) udn = xml_element(xml, "UDN");
    out.name = name.empty() ? out.host : name;
    out.udn = udn.empty() ? location : udn;
    return true;
}

std::string didl_lite_item(const std::string& title, const std::string& artist, const std::string& album,
                           const std::string& url, int64_t duration_ms)
{
    std::string d = "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
                    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                    "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
                    "xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\">"
                    "<item id=\"0\" parentID=\"-1\" restricted=\"1\">";
    d += "<dc:title>" + xml_escape(title.empty() ? url : title) + "</dc:title>";
    if (!artist.empty()) d += "<upnp:artist>" + xml_escape(artist) + "</upnp:artist>";
    if (!artist.empty()) d += "<dc:creator>" + xml_escape(artist) + "</dc:creator>";
    if (!album.empty()) d += "<upnp:album>" + xml_escape(album) + "</upnp:album>";
    d += "<upnp:class>object.item.audioItem.musicTrack</upnp:class>";
    d += "<res protocolInfo=\"" + std::string(kMp3ProtocolInfo) + "\"";
    if (duration_ms >= 0) d += " duration=\"" + format_upnp_time(duration_ms) + "\"";
    d += ">" + xml_escape(url) + "</res></item></DIDL-Lite>";
    return d;
}

SoapResult soap_call(const std::string& control_url, const std::string& service_type, const std::string& action,
                     const std::vector<std::pair<std::string, std::string>>& args, int timeout_ms)
{
    SoapResult result;
    auto url = net::parse_url(control_url);
    if (!url) {
        result.error = "bad control URL: " + control_url;
        return result;
    }
    std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                       "<u:" + action + " xmlns:u=\"" + service_type + "\">";
    for (const auto& [k, v] : args) body += "<" + k + ">" + xml_escape(v) + "</" + k + ">";
    body += "</u:" + action + "></s:Body></s:Envelope>";

    net::Headers headers = {
        {"Content-Type", "text/xml; charset=\"utf-8\""},
        {"SOAPACTION", "\"" + service_type + "#" + action + "\""},
    };
    std::string error;
    auto resp = net::http_request("POST", *url, headers, body, timeout_ms, &error);
    if (!resp) {
        result.error = action + ": " + error;
        return result;
    }
    result.http_status = resp->status;
    result.body = std::move(resp->body);
    if (resp->status / 100 == 2) {
        result.ok = true;
        return result;
    }
    std::string code = xml_element(result.body, "errorCode");
    std::string desc = xml_element(result.body, "errorDescription");
    if (!code.empty()) result.upnp_error = std::atoi(code.c_str());
    result.error = action + ": HTTP " + std::to_string(resp->status);
    if (!code.empty()) result.error += ", UPnP error " + code;
    if (!desc.empty()) result.error += " (" + desc + ")";
    return result;
}

// ---------------------------------------------------------------------------
// RendererBrowser

struct RendererBrowser::Impl {
    WakeFn wake;
    int64_t expiry_ms;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<Renderer> renderers;
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> searching{false};
    bool refresh_requested = false;
    int listen_ms = 2500;
    bool quit = false;
    std::thread thread;

    void run();
    void search(int listen_ms);
    void note(const Renderer& r);
};

RendererBrowser::RendererBrowser(WakeFn wake, int64_t expiry_ms) : impl_(std::make_unique<Impl>())
{
    impl_->wake = std::move(wake);
    impl_->expiry_ms = expiry_ms;
    impl_->thread = std::thread([this] { impl_->run(); });
}

RendererBrowser::~RendererBrowser()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->quit = true;
    }
    impl_->cv.notify_all();
    impl_->thread.join();
}

void RendererBrowser::refresh(int listen_ms)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->searching) return;
        impl_->refresh_requested = true;
        impl_->listen_ms = listen_ms;
        impl_->searching = true;
    }
    impl_->cv.notify_all();
}

bool RendererBrowser::searching() const
{
    return impl_->searching;
}

std::vector<Renderer> RendererBrowser::renderers() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->renderers;
}

uint64_t RendererBrowser::generation() const
{
    return impl_->generation;
}

void RendererBrowser::Impl::run()
{
    for (;;) {
        int ms;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return quit || refresh_requested; });
            if (quit) return;
            refresh_requested = false;
            ms = listen_ms;
        }
        search(ms);
        searching = false;
        if (wake) wake();
    }
}

void RendererBrowser::Impl::note(const Renderer& found)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = std::find_if(renderers.begin(), renderers.end(), [&](const Renderer& r) { return r.udn == found.udn; });
        if (it == renderers.end()) {
            renderers.push_back(found);
            std::sort(renderers.begin(), renderers.end(), [](const Renderer& a, const Renderer& b) { return a.name < b.name; });
            changed = true;
        } else {
            if (it->name != found.name || it->control_url != found.control_url) changed = true;
            it->name = found.name;
            it->location = found.location;
            it->control_url = found.control_url;
            it->host = found.host;
            it->last_seen_ms = found.last_seen_ms;
        }
    }
    if (changed) {
        ++generation;
        if (wake) wake();
    }
}

void RendererBrowser::Impl::search(int listen_ms_)
{
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    int ttl = 2;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);

    const char* msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
        "USER-AGENT: pyjuke/1.0 UPnP/1.0\r\n"
        "\r\n";
    size_t len = std::strlen(msearch);
    // Twice, as the spec suggests; UDP on Wi-Fi loses one now and then.
    for (int i = 0; i < 2; ++i) ::sendto(fd, msearch, len, 0, reinterpret_cast<sockaddr*>(&group), sizeof group);

    std::vector<std::string> locations;  // fetched after listening, so replies aren't missed
    int64_t deadline = steady_ms() + listen_ms_;
    char buf[4096];
    for (;;) {
        int64_t left = deadline - steady_ms();
        if (left <= 0) break;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (quit) break;
        }
        pollfd p{fd, POLLIN, 0};
        int r = ::poll(&p, 1, static_cast<int>(std::min<int64_t>(left, 200)));
        if (r <= 0) continue;
        ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
        if (n <= 0) continue;
        std::string_view datagram(buf, static_cast<size_t>(n));
        if (datagram.substr(0, 12) != "HTTP/1.1 200") continue;
        net::Headers h = parse_ssdp_headers(datagram);
        std::string location = net::header_value(h, "LOCATION");
        if (location.empty() || std::find(locations.begin(), locations.end(), location) != locations.end()) continue;
        locations.push_back(location);
    }
    ::close(fd);

    for (const std::string& location : locations) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (quit) return;
        }
        auto url = net::parse_url(location);
        if (!url) continue;
        auto resp = net::http_request("GET", *url, {}, {}, 3000);
        if (!resp || resp->status != 200) continue;
        Renderer r;
        if (!parse_device_description(resp->body, location, r)) continue;
        r.last_seen_ms = steady_ms();
        note(r);
    }

    // Age out renderers that stopped answering.
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        int64_t now = steady_ms();
        size_t before = renderers.size();
        renderers.erase(std::remove_if(renderers.begin(), renderers.end(),
                                       [&](const Renderer& r) { return now - r.last_seen_ms > expiry_ms; }),
                        renderers.end());
        changed = renderers.size() != before;
    }
    if (changed) ++generation;
}

} // namespace pyjuke::upnp
