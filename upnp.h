// upnp.h - the control-point side of UPnP AV, as much as pushing a track to
// a MediaRenderer needs: SSDP discovery of renderers, their AVTransport
// control URL from the device description, and SOAP action calls.
//
// pyjuke is never itself a UPnP device; it only finds and drives them.

#ifndef PYJUKE_UPNP_H
#define PYJUKE_UPNP_H

#include "net.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pyjuke::upnp {

// ---- small pure helpers (tested on their own) ----

// Text of the first <tag ...>...</tag> in `xml`, entities decoded; empty if
// absent. Namespace prefixes on `tag` are matched literally, so pass
// "RelTime" for <RelTime> and "u:Foo" for <u:Foo>.
std::string xml_element(std::string_view xml, std::string_view tag);
// The raw substring of the first <tag ...>...</tag> element (inclusive).
std::string_view xml_element_raw(std::string_view xml, std::string_view tag);
std::string xml_escape(std::string_view text);
std::string xml_unescape(std::string_view text);

// "H:MM:SS", "H:MM:SS.mmm" or "H:MM:SS.f/d" -> milliseconds; -1 for
// "NOT_IMPLEMENTED", empty or garbage.
int64_t parse_upnp_time(std::string_view text);
// milliseconds -> "H:MM:SS" (truncated to whole seconds; "0:00:00" for < 0).
std::string format_upnp_time(int64_t ms);

// Header lines of an SSDP datagram (the request/status line is skipped).
net::Headers parse_ssdp_headers(std::string_view datagram);

// A renderer as the picker shows it and as RemoteRenderer drives it.
struct Renderer {
    std::string udn;          // "uuid:..." identity, stable across restarts
    std::string name;         // friendlyName
    std::string location;     // description URL (also identifies the host)
    std::string control_url;  // absolute AVTransport control URL
    std::string volume_url;   // absolute RenderingControl control URL, or empty
    std::string host;         // host part of control_url
    int64_t last_seen_ms = 0; // steady clock, maintained by RendererBrowser
};

// Fill `out` from a device description document fetched from `location`.
// Finds the first AVTransport service, in the root or an embedded device,
// and the RenderingControl service beside it (volume), if there is one.
// False if the document has no AVTransport service.
bool parse_device_description(std::string_view xml, const std::string& location, Renderer& out);

// DIDL-Lite for one MP3 item, as SetAVTransportURI's CurrentURIMetaData
// (before SOAP escaping). Empty title/artist/album fields are omitted.
std::string didl_lite_item(const std::string& title, const std::string& artist, const std::string& album,
                           const std::string& url, int64_t duration_ms = -1);

// MP3 over HTTP, seekable by byte range.
extern const char* const kMp3ProtocolInfo;

// ---- SOAP ----

struct SoapResult {
    bool ok = false;        // transport worked and HTTP status was 2xx
    int http_status = 0;    // 0 if the request never completed
    int upnp_error = 0;     // <errorCode> from a UPnP fault, if any
    std::string error;      // human-readable failure description
    std::string body;       // response body (the SOAP envelope)

    // Convenience: xml_element(body, name).
    std::string arg(std::string_view name) const { return xml_element(body, name); }
};

constexpr const char* kAVTransport = "urn:schemas-upnp-org:service:AVTransport:1";
constexpr const char* kRenderingControl = "urn:schemas-upnp-org:service:RenderingControl:1";

// POST one action to `control_url` and wait up to timeout_ms.
SoapResult soap_call(const std::string& control_url, const std::string& service_type, const std::string& action,
                     const std::vector<std::pair<std::string, std::string>>& args, int timeout_ms);

// ---- discovery ----

// Finds MediaRenderers with SSDP M-SEARCH on a worker thread. refresh()
// starts a search; renderers() is a snapshot the UI can read any time.
// Renderers unseen for `expiry_ms` are dropped at the end of a search.
class RendererBrowser {
public:
    using WakeFn = std::function<void()>;

    // `wake` is called from the worker thread when the renderer list changes
    // or a search finishes (pass glfwPostEmptyEvent).
    explicit RendererBrowser(WakeFn wake = {}, int64_t expiry_ms = 3 * 60 * 1000);
    ~RendererBrowser();
    RendererBrowser(const RendererBrowser&) = delete;
    RendererBrowser& operator=(const RendererBrowser&) = delete;

    // Start a search (M-SEARCH, collect replies for about `listen_ms`). Does
    // nothing if one is already running.
    void refresh(int listen_ms = 2500);
    bool searching() const;

    std::vector<Renderer> renderers() const;
    // Bumped whenever renderers() would differ from the previous snapshot.
    uint64_t generation() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pyjuke::upnp

#endif
