// remote_renderer.h - an AudioControl backed by a UPnP MediaRenderer.
//
// load() tells the renderer to fetch the track from our TrackServer
// (SetAVTransportURI + Play); play/pause/seek are the matching AVTransport
// actions; position and duration come from GetPositionInfo, polled once a
// second on a worker thread and interpolated in between. End of track is
// the renderer going STOPPED near the end of the track; a track the renderer
// refuses, or never starts, is a track error. Volume is the device's own,
// through RenderingControl: read at start and with every poll (so the amp's
// remote shows up on screen), never set unless asked; repeated set_volume()
// calls while a slider is dragged collapse to the latest value.
//
// Like AudioEngine, every public method is called from one controlling
// thread, returns at once, and the wake callback fires from the worker
// whenever a flag the controller should poll has been set.

#ifndef PYJUKE_REMOTE_RENDERER_H
#define PYJUKE_REMOTE_RENDERER_H

#include "audio_control.h"
#include "track_server.h"
#include "upnp.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace pyjuke {

struct RemoteRendererOptions {
    int timeout_ms = 4000;         // per SOAP action
    int poll_ms = 1000;            // GetPositionInfo cadence while loaded
    int start_grace_ms = 12000;    // Play issued but never PLAYING: track error
    int lost_after_failures = 3;   // consecutive unanswered actions: lost()
};

class RemoteRenderer final : public AudioControl {
public:
    using WakeFn = std::function<void()>;
    using Options = RemoteRendererOptions;

    // `server_host` is the address the renderer should fetch tracks from
    // (net::local_address_for(target.host)). Metadata for the DIDL-Lite
    // item is resolved from the file like the Player does.
    RemoteRenderer(upnp::Renderer target, const TrackServer& server, std::string server_host,
                   std::filesystem::path musicdir, WakeFn wake = {}, Options options = {});
    // Sends Stop if anything is loaded, then joins the worker.
    ~RemoteRenderer() override;
    RemoteRenderer(const RemoteRenderer&) = delete;
    RemoteRenderer& operator=(const RemoteRenderer&) = delete;

    // AudioControl
    void load(const std::string& path, bool start_paused) override;
    void play() override;
    void pause() override;
    void seek_ms(int64_t ms) override;
    int64_t position_ms() const override;
    int64_t duration_ms() const override;
    bool take_track_ended() override;
    bool take_track_error() override;
    void set_volume(int percent) override;
    int volume() const override;  // -1 until read, or if the device has no RenderingControl

    const upnp::Renderer& target() const;
    // The renderer has stopped answering altogether; the caller should move
    // playback elsewhere. Latched; also reported through the wake callback.
    bool lost() const;
    // Human-readable description of the most recent failure (empty if none).
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pyjuke

#endif
