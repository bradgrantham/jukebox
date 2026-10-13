// tests/test_network.cpp - net, upnp, TrackServer and RemoteRenderer tests.
// Everything runs over loopback; no real renderer is needed.

#undef NDEBUG
#include <cassert>

#include "audio.h"
#include "audio_switch.h"
#include "local_volume.h"
#include "net.h"
#include "remote_renderer.h"
#include "track_server.h"
#include "upnp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace pyjuke;

namespace {

void section(const char* name) { std::printf("  %s\n", name); }

#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        auto _a = (a);                                                                                \
        auto _b = (b);                                                                                \
        if (!(_a == _b)) {                                                                            \
            std::ostringstream _o;                                                                    \
            _o << _a << " != " << _b;                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: %s\n", __FILE__, __LINE__, #a, #b, \
                         _o.str().c_str());                                                           \
            assert(false);                                                                            \
        }                                                                                             \
    } while (0)

bool wait_for(const std::function<bool()>& pred, int timeout_ms = 3000)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

std::string loopback_url(uint16_t port, const std::string& path)
{
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

void test_url_and_http_parsing()
{
    section("net: url parsing and resolution");
    auto u = net::parse_url("http://192.168.1.20:49152/description.xml");
    assert(u);
    CHECK_EQ(u->host, std::string("192.168.1.20"));
    CHECK_EQ(u->port, 49152);
    CHECK_EQ(u->path, std::string("/description.xml"));
    u = net::parse_url("HTTP://renderer.local");
    assert(u && u->port == 80 && u->path == "/");
    u = net::parse_url("http://[fe80::1]:8080/x?y=1");
    assert(u && u->host == "fe80::1" && u->port == 8080 && u->path == "/x?y=1");
    assert(!net::parse_url("https://a/b"));
    assert(!net::parse_url("http://"));
    assert(!net::parse_url("http://host:0/"));
    assert(!net::parse_url("http://host:99999/"));
    assert(!net::parse_url("ftp://host/"));

    CHECK_EQ(net::resolve_url("http://h:1/a/b.xml", "/ctl"), std::string("http://h:1/ctl"));
    CHECK_EQ(net::resolve_url("http://h:1/a/b.xml", "ctl"), std::string("http://h:1/a/ctl"));
    CHECK_EQ(net::resolve_url("http://h/a/b.xml", "http://x/y"), std::string("http://x/y"));
    CHECK_EQ(net::resolve_url("http://h:1/", "AVTransport/ctrl"), std::string("http://h:1/AVTransport/ctrl"));
    CHECK_EQ(net::resolve_url("http://h:1", "x"), std::string("http://h:1/x"));

    section("net: response/request heads, chunked bodies");
    net::HttpResponse r;
    size_t n = net::parse_response_head("HTTP/1.1 206 Partial Content\r\nContent-Type: audio/mpeg\r\ncontent-length: 10\r\n\r\nbody", r);
    CHECK_EQ(n, size_t(78));
    CHECK_EQ(r.status, 206);
    CHECK_EQ(r.reason, std::string("Partial Content"));
    CHECK_EQ(r.header("CONTENT-LENGTH"), std::string("10"));
    CHECK_EQ(net::parse_response_head("HTTP/1.1 200\r\n", r), size_t(0));   // no blank line yet
    CHECK_EQ(net::parse_response_head("garbage\r\n\r\n", r), size_t(0));
    net::HttpRequest q;
    n = net::parse_request_head("GET /track/3.mp3 HTTP/1.1\r\nHost: x\r\nRange: bytes=0-\r\n\r\n", q);
    assert(n > 0);
    CHECK_EQ(q.method, std::string("GET"));
    CHECK_EQ(q.path, std::string("/track/3.mp3"));
    CHECK_EQ(net::header_value(q.headers, "range"), std::string("bytes=0-"));
    CHECK_EQ(net::parse_request_head("GET\r\n\r\n", q), size_t(0));

    std::string out;
    assert(net::decode_chunked("4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\n\r\n", out));
    CHECK_EQ(out, std::string("Wikipedia"));
    assert(net::decode_chunked("0\r\n\r\n", out) && out.empty());
    assert(!net::decode_chunked("4\r\nWik", out));
    assert(!net::decode_chunked("zz\r\n", out));
}

void test_upnp_helpers()
{
    section("upnp: xml, times, ssdp, didl");
    CHECK_EQ(upnp::xml_element("<a><RelTime>0:01:23</RelTime></a>", "RelTime"), std::string("0:01:23"));
    CHECK_EQ(upnp::xml_element("<x><t attr=\"1\"> a &amp; b &lt;c&gt; &#65;&#x42; </t></x>", "t"), std::string("a & b <c> AB"));
    CHECK_EQ(upnp::xml_element("<x><tt>no</tt><t>yes</t></x>", "t"), std::string("yes"));
    CHECK_EQ(upnp::xml_element("<x><t/></x>", "t"), std::string(""));
    CHECK_EQ(upnp::xml_element("<x></x>", "t"), std::string(""));
    CHECK_EQ(upnp::xml_escape("a<b>&\"'"), std::string("a&lt;b&gt;&amp;&quot;&apos;"));
    CHECK_EQ(upnp::xml_unescape("&amp;lt; &bogus; & x"), std::string("&lt; &bogus; & x"));

    CHECK_EQ(upnp::parse_upnp_time("0:01:23"), int64_t(83000));
    CHECK_EQ(upnp::parse_upnp_time("1:02:03.500"), int64_t(3723500));
    CHECK_EQ(upnp::parse_upnp_time("0:00:01.5"), int64_t(1500));
    CHECK_EQ(upnp::parse_upnp_time("0:00:02.1/4"), int64_t(2250));
    CHECK_EQ(upnp::parse_upnp_time("00:45"), int64_t(45000));
    CHECK_EQ(upnp::parse_upnp_time("7"), int64_t(7000));
    CHECK_EQ(upnp::parse_upnp_time("NOT_IMPLEMENTED"), int64_t(-1));
    CHECK_EQ(upnp::parse_upnp_time(""), int64_t(-1));
    CHECK_EQ(upnp::parse_upnp_time("1:2:3:4"), int64_t(-1));
    CHECK_EQ(upnp::parse_upnp_time("a:b"), int64_t(-1));
    CHECK_EQ(upnp::format_upnp_time(3723500), std::string("1:02:03"));
    CHECK_EQ(upnp::format_upnp_time(-5), std::string("0:00:00"));

    net::Headers h = upnp::parse_ssdp_headers(
        "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nLOCATION: http://192.168.1.5:49152/description.xml\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\nUSN: uuid:abc::urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n");
    CHECK_EQ(net::header_value(h, "location"), std::string("http://192.168.1.5:49152/description.xml"));
    CHECK_EQ(net::header_value(h, "usn"), std::string("uuid:abc::urn:schemas-upnp-org:device:MediaRenderer:1"));

    std::string didl = upnp::didl_lite_item("A & B", "Art <1>", "", "http://h:1/track/7.mp3", 61000);
    assert(didl.find("<dc:title>A &amp; B</dc:title>") != std::string::npos);
    assert(didl.find("<upnp:artist>Art &lt;1&gt;</upnp:artist>") != std::string::npos);
    assert(didl.find("<upnp:album>") == std::string::npos);
    assert(didl.find("duration=\"0:01:01\"") != std::string::npos);
    assert(didl.find(upnp::kMp3ProtocolInfo) != std::string::npos);
    assert(didl.find("http://h:1/track/7.mp3</res>") != std::string::npos);

    section("upnp: device descriptions (root and embedded renderer)");
    const char* root_renderer =
        "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion>"
        "<device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
        "<friendlyName>Living Room</friendlyName><UDN>uuid:11111111-2222-3333-4444-555555555555</UDN>"
        "<serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
        "<controlURL>/upnp/control/rendering_control</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
        "<controlURL>/upnp/control/rendertransport1</controlURL>"
        "<eventSubURL>/upnp/event/rendertransport1</eventSubURL>"
        "<SCPDURL>/upnp/rendertransportSCPD.xml</SCPDURL></service>"
        "</serviceList></device></root>";
    upnp::Renderer r;
    assert(upnp::parse_device_description(root_renderer, "http://192.168.1.5:49152/description.xml", r));
    CHECK_EQ(r.name, std::string("Living Room"));
    CHECK_EQ(r.udn, std::string("uuid:11111111-2222-3333-4444-555555555555"));
    CHECK_EQ(r.control_url, std::string("http://192.168.1.5:49152/upnp/control/rendertransport1"));
    CHECK_EQ(r.volume_url, std::string("http://192.168.1.5:49152/upnp/control/rendering_control"));
    CHECK_EQ(r.host, std::string("192.168.1.5"));

    const char* embedded =
        "<root><URLBase>http://10.0.0.9:8080/base/</URLBase>"
        "<device><deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>"
        "<friendlyName>Pioneer Root</friendlyName><UDN>uuid:root</UDN>"
        "<serviceList><service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>"
        "<controlURL>cd/ctl</controlURL></service></serviceList>"
        "<deviceList><device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:2</deviceType>"
        "<friendlyName>Pioneer VSX</friendlyName><UDN>uuid:renderer</UDN>"
        "<serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:2</serviceType>"
        "<controlURL>avt/ctl</controlURL></service></serviceList>"
        "</device></deviceList></device></root>";
    upnp::Renderer e;
    assert(upnp::parse_device_description(embedded, "http://10.0.0.9:8080/desc.xml", e));
    CHECK_EQ(e.name, std::string("Pioneer VSX"));
    CHECK_EQ(e.udn, std::string("uuid:renderer"));
    CHECK_EQ(e.control_url, std::string("http://10.0.0.9:8080/base/avt/ctl"));
    assert(e.volume_url.empty());  // no RenderingControl: volume unsupported

    upnp::Renderer none;
    assert(!upnp::parse_device_description("<root><device><friendlyName>TV</friendlyName></device></root>", "http://h/d.xml", none));
}

void test_track_server(const fs::path& tmp)
{
    section("track server: ranges, errors, index/url mapping");
    fs::create_directories(tmp / "lib");
    std::string content(1000, '\0');
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>(i % 251);
    fs::path a = tmp / "lib" / "a.mp3", b = tmp / "lib" / "b.mp3";
    {
        std::ofstream(a, std::ios::binary) << content;
        std::ofstream(b, std::ios::binary) << "tiny";
    }
    TrackServer server({a, b});
    CHECK_EQ(server.index_of(a).value_or(99), size_t(0));
    CHECK_EQ(server.index_of(b).value_or(99), size_t(1));
    assert(!server.index_of(tmp / "lib" / "c.mp3"));
    CHECK_EQ(TrackServer::index_from_url("http://1.2.3.4:5/track/17.mp3").value_or(99), size_t(17));
    assert(!TrackServer::index_from_url("http://1.2.3.4:5/track/x.mp3"));
    assert(!TrackServer::index_from_url("http://1.2.3.4:5/other/1.mp3"));

    auto get = [&](const char* method, const char* path, const char* range) {
        net::HttpRequest req;
        req.method = method;
        req.path = path;
        if (range) req.headers.emplace_back("Range", range);
        return server.handle(req);
    };
    net::HttpResponse resp = get("GET", "/track/0.mp3", nullptr);
    CHECK_EQ(resp.status, 200);
    CHECK_EQ(resp.file, a.string());
    CHECK_EQ(resp.file_offset, uint64_t(0));
    CHECK_EQ(resp.file_length, uint64_t(1000));
    CHECK_EQ(resp.header("Content-Type"), std::string("audio/mpeg"));
    CHECK_EQ(resp.header("Accept-Ranges"), std::string("bytes"));
    assert(resp.header("contentFeatures.dlna.org").find("DLNA.ORG_OP=01") != std::string::npos);

    resp = get("GET", "/track/0.mp3", "bytes=100-199");
    CHECK_EQ(resp.status, 206);
    CHECK_EQ(resp.file_offset, uint64_t(100));
    CHECK_EQ(resp.file_length, uint64_t(100));
    CHECK_EQ(resp.header("Content-Range"), std::string("bytes 100-199/1000"));
    resp = get("GET", "/track/0.mp3", "bytes=900-");
    CHECK_EQ(resp.status, 206);
    CHECK_EQ(resp.file_offset, uint64_t(900));
    CHECK_EQ(resp.file_length, uint64_t(100));
    resp = get("GET", "/track/0.mp3", "bytes=-50");
    CHECK_EQ(resp.status, 206);
    CHECK_EQ(resp.file_offset, uint64_t(950));
    CHECK_EQ(resp.file_length, uint64_t(50));
    resp = get("GET", "/track/0.mp3", "bytes=0-5000");  // end clamped
    CHECK_EQ(resp.status, 206);
    CHECK_EQ(resp.file_length, uint64_t(1000));
    resp = get("GET", "/track/0.mp3", "bytes=1000-");
    CHECK_EQ(resp.status, 416);
    CHECK_EQ(resp.header("Content-Range"), std::string("bytes */1000"));
    resp = get("GET", "/track/0.mp3", "bytes=5-2");
    CHECK_EQ(resp.status, 416);
    resp = get("GET", "/track/0.mp3", "bytes=0-1,5-6");
    CHECK_EQ(resp.status, 416);
    CHECK_EQ(get("GET", "/track/2.mp3", nullptr).status, 404);
    CHECK_EQ(get("GET", "/track/0.ogg", nullptr).status, 404);
    CHECK_EQ(get("GET", "/track/../../etc/passwd", nullptr).status, 404);
    CHECK_EQ(get("GET", "/", nullptr).status, 404);
    CHECK_EQ(get("POST", "/track/0.mp3", nullptr).status, 405);
    CHECK_EQ(get("HEAD", "/track/1.mp3", nullptr).status, 200);

    section("track server + http client over loopback");
    std::string err;
    assert(server.start(0, &err));
    assert(server.port() != 0);
    std::string url = server.url_for(0, "127.0.0.1");
    CHECK_EQ(url, loopback_url(server.port(), "/track/0.mp3"));
    auto u = net::parse_url(url);
    assert(u);
    auto got = net::http_request("GET", *u, {}, {}, 3000, &err);
    assert(got);
    CHECK_EQ(got->status, 200);
    CHECK_EQ(got->body.size(), size_t(1000));
    assert(got->body == content);
    got = net::http_request("GET", *u, {{"Range", "bytes=10-19"}}, {}, 3000, &err);
    assert(got && got->status == 206);
    assert(got->body == content.substr(10, 10));
    CHECK_EQ(got->header("Content-Length"), std::string("10"));
    got = net::http_request("HEAD", *u, {}, {}, 3000, &err);
    assert(got && got->status == 200 && got->body.empty());
    CHECK_EQ(got->header("Content-Length"), std::string("1000"));
    got = net::http_request("GET", *net::parse_url(loopback_url(server.port(), "/track/9.mp3")), {}, {}, 3000, &err);
    assert(got && got->status == 404);
    // Several connections at once.
    std::vector<std::thread> clients;
    std::atomic<int> ok{0};
    for (int i = 0; i < 6; ++i)
        clients.emplace_back([&] {
            auto g = net::http_request("GET", *u, {}, {}, 3000);
            if (g && g->status == 200 && g->body == content) ++ok;
        });
    for (auto& t : clients) t.join();
    CHECK_EQ(ok.load(), 6);
    server.stop();
    assert(!net::http_request("GET", *u, {}, {}, 500, &err));

    // A generic handler: echoing POST bodies, and a connection refused.
    net::HttpServer echo;
    assert(echo.start(0, [](const net::HttpRequest& req) {
        net::HttpResponse r;
        r.status = 200;
        r.headers.emplace_back("Content-Type", "text/xml");
        r.body = req.method + " " + req.path + " " + net::header_value(req.headers, "SOAPACTION") + " " + req.body;
        return r;
    }, &err));
    auto e = net::http_request("POST", *net::parse_url(loopback_url(echo.port(), "/ctl")), {{"SOAPACTION", "\"x#Play\""}}, "<body/>", 3000, &err);
    assert(e && e->status == 200);
    CHECK_EQ(e->body, std::string("POST /ctl \"x#Play\" <body/>"));
    CHECK_EQ(e->header("content-type"), std::string("text/xml"));
}

// A scripted AVTransport service: records the actions it receives and
// answers status queries from `state`, `rel_ms`, `dur_ms`.
struct FakeRenderer {
    net::HttpServer http;
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> actions;  // name, request body
    std::string state = "STOPPED";
    int64_t rel_ms = 0;
    int64_t dur_ms = -1;
    int fault_on_set_uri = 0;  // UPnP error code to return from SetAVTransportURI, 0 = none
    int volume = 37;           // RenderingControl Master volume

    bool start()
    {
        return http.start(0, [this](const net::HttpRequest& req) { return handle(req); });
    }
    std::string control_url() const { return loopback_url(http.port(), "/AVTransport/ctl"); }
    std::string volume_url() const { return loopback_url(http.port(), "/RenderingControl/ctl"); }

    net::HttpResponse handle(const net::HttpRequest& req)
    {
        std::string soapaction = net::header_value(req.headers, "SOAPACTION");
        size_t hash = soapaction.find('#');
        std::string action = hash == std::string::npos ? "" : soapaction.substr(hash + 1);
        if (!action.empty() && action.back() == '"') action.pop_back();
        std::lock_guard<std::mutex> lock(mutex);
        actions.emplace_back(action, req.body);
        net::HttpResponse r;
        r.headers.emplace_back("Content-Type", "text/xml; charset=\"utf-8\"");
        std::string inner;
        if (action == "SetAVTransportURI" && fault_on_set_uri != 0) {
            r.status = 500;
            r.body = "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>" + std::to_string(fault_on_set_uri) +
                     "</errorCode><errorDescription>Resource not found</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
            return r;
        }
        if (req.path == "/RenderingControl/ctl") {
            if (action == "GetVolume") inner = "<CurrentVolume>" + std::to_string(volume) + "</CurrentVolume>";
            else if (action == "SetVolume") volume = std::atoi(upnp::xml_element(req.body, "DesiredVolume").c_str());
        } else if (action == "GetTransportInfo") {
            inner = "<CurrentTransportState>" + state + "</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>";
        } else if (action == "GetPositionInfo") {
            inner = "<Track>1</Track><TrackDuration>" + (dur_ms < 0 ? std::string("NOT_IMPLEMENTED") : upnp::format_upnp_time(dur_ms)) +
                    "</TrackDuration><RelTime>" + upnp::format_upnp_time(rel_ms) + "</RelTime><AbsTime>NOT_IMPLEMENTED</AbsTime>";
        }
        r.status = 200;
        // Chunked, as some renderers answer.
        std::string body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:" + action +
                           "Response xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">" + inner + "</u:" + action + "Response></s:Body></s:Envelope>";
        r.body = body;
        return r;
    }

    int count(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        int n = 0;
        for (auto& a : actions) n += a.first == name;
        return n;
    }
    std::string last_body(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = actions.rbegin(); it != actions.rend(); ++it)
            if (it->first == name) return it->second;
        return {};
    }
    void set(const char* st, int64_t rel, int64_t dur)
    {
        std::lock_guard<std::mutex> lock(mutex);
        state = st;
        rel_ms = rel;
        dur_ms = dur;
    }
};

void test_remote_renderer(const fs::path& tmp)
{
    section("remote renderer: load/play/pause/seek, position, end, errors, lost");
    fs::create_directories(tmp / "rlib");
    fs::path a = tmp / "rlib" / "Artist - Song One.mp3", b = tmp / "rlib" / "second.mp3";
    std::ofstream(a, std::ios::binary) << "mp3?";
    std::ofstream(b, std::ios::binary) << "mp3?";
    TrackServer server({a, b});
    assert(server.start(0));

    FakeRenderer fake;
    assert(fake.start());
    upnp::Renderer target;
    target.udn = "uuid:fake";
    target.name = "Fake";
    target.host = "127.0.0.1";
    target.control_url = fake.control_url();
    target.volume_url = fake.volume_url();

    std::atomic<int> wakes{0};
    RemoteRendererOptions opts;
    opts.timeout_ms = 2000;
    opts.poll_ms = 50;
    opts.start_grace_ms = 600;
    opts.lost_after_failures = 3;
    {
        RemoteRenderer rr(target, server, "127.0.0.1", tmp / "rlib", [&] { ++wakes; }, opts);
        CHECK_EQ(rr.position_ms(), int64_t(-1));
        CHECK_EQ(rr.duration_ms(), int64_t(-1));

        // Volume is read from the device before anything plays, never set.
        assert(wait_for([&] { return rr.volume() == 37; }));
        CHECK_EQ(fake.count("SetVolume"), 0);
        rr.set_volume(60);
        CHECK_EQ(rr.volume(), 60);  // optimistic
        assert(wait_for([&] { return fake.count("SetVolume") >= 1; }));
        std::string vol_body = fake.last_body("SetVolume");
        CHECK_EQ(upnp::xml_element(vol_body, "DesiredVolume"), std::string("60"));
        CHECK_EQ(upnp::xml_element(vol_body, "Channel"), std::string("Master"));
        CHECK_EQ(upnp::xml_element(vol_body, "InstanceID"), std::string("0"));
        rr.set_volume(250);
        assert(wait_for([&] { return fake.count("SetVolume") >= 2; }));
        CHECK_EQ(upnp::xml_element(fake.last_body("SetVolume"), "DesiredVolume"), std::string("100"));
        CHECK_EQ(rr.volume(), 100);

        rr.load(a.string(), false);
        assert(wait_for([&] { return fake.count("Play") >= 1; }));
        CHECK_EQ(fake.count("SetAVTransportURI"), 1);
        std::string set_body = fake.last_body("SetAVTransportURI");
        std::string uri = upnp::xml_element(set_body, "CurrentURI");
        CHECK_EQ(uri, server.url_for(0, "127.0.0.1"));
        std::string meta = upnp::xml_element(set_body, "CurrentURIMetaData");  // unescaped once: DIDL-Lite
        assert(meta.find("<dc:title>Song One</dc:title>") != std::string::npos);
        assert(meta.find("<upnp:artist>Artist</upnp:artist>") != std::string::npos);
        assert(upnp::xml_element(set_body, "InstanceID") == "0");
        // Nothing reported before the renderer says PLAYING.
        assert(!rr.take_track_ended());
        assert(!rr.take_track_error());

        fake.set("PLAYING", 5000, 60000);
        assert(wait_for([&] { return rr.duration_ms() == 60000 && rr.position_ms() >= 5000; }));
        int64_t p1 = rr.position_ms();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        assert(rr.position_ms() >= p1 + 80);  // interpolates between polls

        rr.seek_ms(30000);
        assert(wait_for([&] { return fake.count("Seek") >= 1; }));
        std::string seek_body = fake.last_body("Seek");
        CHECK_EQ(upnp::xml_element(seek_body, "Unit"), std::string("REL_TIME"));
        CHECK_EQ(upnp::xml_element(seek_body, "Target"), std::string("0:00:30"));
        fake.set("PLAYING", 30000, 60000);

        rr.pause();
        assert(wait_for([&] { return fake.count("Pause") >= 1; }));
        fake.set("PAUSED_PLAYBACK", 31000, 60000);
        assert(wait_for([&] { return rr.position_ms() == 31000; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK_EQ(rr.position_ms(), int64_t(31000));  // frozen while paused

        rr.play();
        assert(wait_for([&] { return fake.count("Play") >= 2; }));
        fake.set("PLAYING", 31000, 60000);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        assert(rr.position_ms() > 31000);

        // A change made with the device's own remote shows up after the hold.
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.volume = 20;
        }
        assert(wait_for([&] { return rr.volume() == 20; }, 3000));
        // A burst, as from a slider drag: ends at the last value.
        int sets = fake.count("SetVolume");
        for (int v = 0; v <= 100; v += 5) rr.set_volume(v);
        assert(wait_for([&] { return fake.count("SetVolume") > sets && upnp::xml_element(fake.last_body("SetVolume"), "DesiredVolume") == "100"; }));
        CHECK_EQ(rr.volume(), 100);
        assert(fake.count("SetVolume") - sets < 21);  // coalesced at least a little

        // Stopped early from the renderer's own controls: no end-of-track.
        fake.set("STOPPED", 32000, 60000);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        assert(!rr.take_track_ended());
        assert(!rr.take_track_error());
        int64_t frozen = rr.position_ms();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK_EQ(rr.position_ms(), frozen);

        // Play again, then played to the end.
        rr.play();
        assert(wait_for([&] { return fake.count("Play") >= 3; }));
        fake.set("PLAYING", 58000, 60000);
        assert(wait_for([&] { return rr.position_ms() >= 58000; }));
        int wakes_before = wakes;
        fake.set("STOPPED", 0, 60000);
        assert(wait_for([&] { return rr.take_track_ended(); }));
        assert(wakes > wakes_before);
        CHECK_EQ(rr.position_ms(), int64_t(60000));
        // Only once.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        assert(!rr.take_track_ended());

        // Loading paused does not Play; a later play() does.
        int plays = fake.count("Play");
        rr.load(b.string(), true);
        assert(wait_for([&] { return fake.count("SetAVTransportURI") >= 2; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_EQ(fake.count("Play"), plays);
        CHECK_EQ(rr.position_ms(), int64_t(0));
        rr.seek_ms(10000);  // before playing: remembered, applied once PLAYING
        rr.play();
        assert(wait_for([&] { return fake.count("Play") == plays + 1; }));
        fake.set("PLAYING", 0, 60000);
        assert(wait_for([&] { return fake.count("Seek") >= 2; }));
        CHECK_EQ(upnp::xml_element(fake.last_body("Seek"), "Target"), std::string("0:00:10"));

        // A path outside the served library is a track error.
        rr.load((tmp / "rlib" / "missing.mp3").string(), false);
        assert(wait_for([&] { return rr.take_track_error(); }));
        assert(rr.last_error().find("not in the library") != std::string::npos);

        // A renderer refusing the URI is a track error with the UPnP code in the text.
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.fault_on_set_uri = 716;
        }
        rr.load(a.string(), false);
        assert(wait_for([&] { return rr.take_track_error(); }));
        assert(rr.last_error().find("716") != std::string::npos);
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.fault_on_set_uri = 0;
        }

        // Never starts playing: error after the grace period.
        fake.set("STOPPED", 0, -1);
        rr.load(a.string(), false);
        assert(wait_for([&] { return fake.count("Play") >= plays + 2; }));
        assert(!wait_for([&] { return rr.take_track_error(); }, 300));
        assert(wait_for([&] { return rr.take_track_error(); }, 1500));
        assert(rr.last_error().find("did not start") != std::string::npos);
        assert(!rr.lost());
    }
    // Destruction sent Stop.
    assert(fake.count("Stop") >= 1);

    // Without RenderingControl, volume stays unknown and set_volume is a no-op.
    {
        upnp::Renderer mute = target;
        mute.volume_url.clear();
        int sets = fake.count("SetVolume");
        RemoteRenderer rr(mute, server, "127.0.0.1", tmp / "rlib", [&] { ++wakes; }, opts);
        rr.set_volume(50);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_EQ(rr.volume(), -1);
        CHECK_EQ(fake.count("SetVolume"), sets);
    }

    // The renderer disappears: lost(), not a stream of track errors.
    {
        RemoteRenderer rr(target, server, "127.0.0.1", tmp / "rlib", [&] { ++wakes; }, opts);
        rr.load(a.string(), false);
        assert(wait_for([&] { return fake.count("Play") >= 1 && fake.count("Stop") >= 2; }));
        fake.set("PLAYING", 1000, 60000);
        assert(wait_for([&] { return rr.position_ms() >= 1000; }));
        fake.http.stop();
        assert(wait_for([&] { return rr.lost(); }, 4000));
        assert(rr.last_error().find("not answering") != std::string::npos);
        assert(!rr.take_track_error());
    }
}

void test_audio_switch()
{
    section("audio switch forwards to the current target");
    struct Counter : AudioControl {
        int loads = 0, plays = 0, pauses = 0, seeks = 0, vol = 70;
        void load(const std::string&, bool) override { ++loads; }
        void play() override { ++plays; }
        void pause() override { ++pauses; }
        void seek_ms(int64_t) override { ++seeks; }
        int64_t position_ms() const override { return 11; }
        int64_t duration_ms() const override { return 22; }
        bool take_track_ended() override { return false; }
        bool take_track_error() override { return false; }
        void set_volume(int v) override { vol = v; }
        int volume() const override { return vol; }
    } a, b;
    AudioSwitch sw(a);
    sw.load("x", false);
    sw.play();
    CHECK_EQ(a.loads, 1);
    CHECK_EQ(a.plays, 1);
    sw.set_target(b);
    sw.pause();
    sw.seek_ms(5);
    CHECK_EQ(b.pauses, 1);
    CHECK_EQ(b.seeks, 1);
    CHECK_EQ(a.pauses, 0);
    CHECK_EQ(sw.position_ms(), int64_t(11));
    assert(&sw.target() == &b);
    sw.set_volume(33);
    CHECK_EQ(b.vol, 33);
    CHECK_EQ(a.vol, 70);
    CHECK_EQ(sw.volume(), 33);

    section("local gain: squared curve, rounding, clipping");
    int16_t s[5] = {1000, -1000, 32767, -32768, 3};
    scale_samples(s, 5, 1.0f);
    CHECK_EQ(s[0], int16_t(1000));
    scale_samples(s, 5, 0.5f);
    CHECK_EQ(s[0], int16_t(500));
    CHECK_EQ(s[1], int16_t(-500));
    CHECK_EQ(s[2], int16_t(16384));
    CHECK_EQ(s[3], int16_t(-16384));
    CHECK_EQ(s[4], int16_t(2));  // 1.5 rounds away from zero
    int16_t loud[2] = {30000, -30000};
    scale_samples(loud, 2, 2.0f);
    CHECK_EQ(loud[0], int16_t(32767));
    CHECK_EQ(loud[1], int16_t(-32768));
    AudioEngine engine;
    CHECK_EQ(engine.volume(), 100);
    assert(engine.volume_backend().empty());
    engine.set_volume(-5);
    CHECK_EQ(engine.volume(), 0);
    engine.set_volume(140);
    CHECK_EQ(engine.volume(), 100);

    section("local volume: a system backend replaces the gain");
    struct FakeSystem : LocalVolume {
        int level = 42;
        int sets = 0;
        int volume() override { return level; }
        bool set_volume(int percent) override { level = percent; ++sets; return true; }
        std::string name() const override { return "fake mixer"; }
    };
    auto fake = std::make_unique<FakeSystem>();
    FakeSystem* raw = fake.get();
    engine.set_volume(30);
    engine.use_system_volume(std::move(fake));
    CHECK_EQ(engine.volume_backend(), std::string("fake mixer"));
    CHECK_EQ(engine.volume(), 42);  // the device's level, not the gain
    engine.set_volume(77);
    CHECK_EQ(raw->level, 77);
    CHECK_EQ(raw->sets, 1);
    CHECK_EQ(engine.volume(), 77);
    engine.set_volume(130);
    CHECK_EQ(raw->level, 100);
    // The platform backend may or may not exist here; opening it must be safe,
    // and reading it must not change anything.
    if (auto real = LocalVolume::open()) {
        int v = real->volume();
        assert(v >= -1 && v <= 100);
        assert(!real->name().empty());
        std::printf("    (platform backend: %s, level %d)\n", real->name().c_str(), v);
    } else {
        std::printf("    (no platform backend here)\n");
    }
}

} // namespace

void run_network_tests(const fs::path& tmp)
{
    test_url_and_http_parsing();
    test_upnp_helpers();
    test_track_server(tmp);
    test_remote_renderer(tmp);
    test_audio_switch();
}
