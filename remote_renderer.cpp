// remote_renderer.cpp - see remote_renderer.h.

#include "remote_renderer.h"

#include "library.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

namespace fs = std::filesystem;

namespace pyjuke {

namespace {

int64_t steady_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr int64_t kEndSlackMs = 5000;  // STOPPED this close to the end counts as "played to the end"

struct CmdLoad { std::string path; bool paused; };
struct CmdPlay {};
struct CmdPause {};
struct CmdSeek { int64_t ms; };
struct CmdQuit {};
using Command = std::variant<CmdLoad, CmdPlay, CmdPause, CmdSeek, CmdQuit>;

} // namespace

struct RemoteRenderer::Impl {
    Impl(upnp::Renderer t, const TrackServer& s, std::string host, fs::path md, WakeFn w, Options o)
        : target(std::move(t)), server(s), server_host(std::move(host)), musicdir(std::move(md)), wake(std::move(w)), options(o)
    {
    }

    upnp::Renderer target;
    const TrackServer& server;
    std::string server_host;
    fs::path musicdir;
    WakeFn wake;
    Options options;

    // Command queue (controller -> worker).
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Command> queue;

    // Published status (worker -> controller).
    mutable std::mutex status_mutex;
    bool loaded = false;        // something is on the renderer
    bool interpolating = false; // position advances with the clock
    int64_t base_ms = 0;        // position at base_time
    int64_t base_time = 0;
    int64_t duration = -1;
    std::string error_text;
    std::atomic<bool> ended{false};
    std::atomic<bool> error{false};
    std::atomic<bool> lost{false};
    std::atomic<uint64_t> generation{0};  // bumped by load(); stale results are dropped

    // Worker-private state.
    std::thread thread;
    bool w_loaded = false;
    bool w_want_playing = false;
    bool w_saw_playing = false;
    bool w_finished = false;      // ended or failed; stop polling until the next load
    int64_t w_play_issued = 0;
    std::optional<int64_t> w_pending_seek;
    int64_t w_next_poll = 0;
    int w_failures = 0;
    uint64_t w_generation = 0;

    void post(Command c);
    void run();
    bool handle(Command& c);  // false on quit
    void do_load(const CmdLoad& c);
    void poll();

    upnp::SoapResult action(const char* name, std::vector<std::pair<std::string, std::string>> args = {});
    void publish_position(int64_t pos, bool playing);
    void fail(const std::string& why, bool track_error);
    void set_wake_flag(std::atomic<bool>& flag);
};

RemoteRenderer::RemoteRenderer(upnp::Renderer target, const TrackServer& server, std::string server_host,
                               fs::path musicdir, WakeFn wake, Options options)
    : impl_(std::make_unique<Impl>(std::move(target), server, std::move(server_host), std::move(musicdir), std::move(wake), options))
{
    impl_->thread = std::thread([this] { impl_->run(); });
}

RemoteRenderer::~RemoteRenderer()
{
    impl_->post(CmdQuit{});
    impl_->thread.join();
}

void RemoteRenderer::Impl::post(Command c)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        queue.push_back(std::move(c));
    }
    queue_cv.notify_all();
}

void RemoteRenderer::load(const std::string& path, bool start_paused)
{
    // Flags from the track being replaced must never reach the controller.
    ++impl_->generation;
    impl_->ended = false;
    impl_->error = false;
    {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->loaded = true;
        impl_->interpolating = false;
        impl_->base_ms = 0;
        impl_->base_time = steady_ms();
        impl_->duration = -1;
    }
    impl_->post(CmdLoad{path, start_paused});
}

void RemoteRenderer::play()
{
    impl_->post(CmdPlay{});
}

void RemoteRenderer::pause()
{
    impl_->post(CmdPause{});
}

void RemoteRenderer::seek_ms(int64_t ms)
{
    impl_->post(CmdSeek{std::max<int64_t>(ms, 0)});
}

int64_t RemoteRenderer::position_ms() const
{
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    if (!impl_->loaded) return -1;
    int64_t pos = impl_->base_ms;
    if (impl_->interpolating) pos += steady_ms() - impl_->base_time;
    if (impl_->duration > 0) pos = std::min(pos, impl_->duration);
    return std::max<int64_t>(pos, 0);
}

int64_t RemoteRenderer::duration_ms() const
{
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    return impl_->loaded ? impl_->duration : -1;
}

bool RemoteRenderer::take_track_ended()
{
    return impl_->ended.exchange(false);
}

bool RemoteRenderer::take_track_error()
{
    return impl_->error.exchange(false);
}

const upnp::Renderer& RemoteRenderer::target() const
{
    return impl_->target;
}

bool RemoteRenderer::lost() const
{
    return impl_->lost;
}

std::string RemoteRenderer::last_error() const
{
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    return impl_->error_text;
}

// ---------------------------------------------------------------------------
// worker

void RemoteRenderer::Impl::set_wake_flag(std::atomic<bool>& flag)
{
    flag = true;
    if (wake) wake();
}

void RemoteRenderer::Impl::publish_position(int64_t pos, bool playing)
{
    std::lock_guard<std::mutex> lock(status_mutex);
    if (w_generation != generation) return;  // a newer load() owns the status now
    const int64_t now = steady_ms();
    const int64_t expected = interpolating ? base_ms + now - base_time : base_ms;
    if (pos >= 0) {
        // Renderers report whole seconds, so a report can trail the clock by
        // up to a second; snapping back to it every poll would make the
        // display jitter. Re-base when the report is ahead of us, when it is
        // further behind than truncation explains (a seek, a restart), or
        // when the clock is not running anyway.
        if (!interpolating || !playing || pos > expected || pos < expected - 1500) {
            base_ms = pos;
            base_time = now;
        }
    } else if (interpolating && !playing) {
        base_ms = std::max<int64_t>(expected, 0);
        base_time = now;
    }
    interpolating = playing;
}

void RemoteRenderer::Impl::fail(const std::string& why, bool track_error)
{
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        error_text = why;
        interpolating = false;
    }
    if (track_error) {
        w_finished = true;
        if (w_generation == generation) set_wake_flag(error);
    }
}

upnp::SoapResult RemoteRenderer::Impl::action(const char* name, std::vector<std::pair<std::string, std::string>> args)
{
    args.insert(args.begin(), {"InstanceID", "0"});
    upnp::SoapResult r = upnp::soap_call(target.control_url, upnp::kAVTransport, name, args, options.timeout_ms);
    if (r.http_status == 0) {
        if (++w_failures >= options.lost_after_failures && !lost) {
            {
                std::lock_guard<std::mutex> lock(status_mutex);
                error_text = target.name + " is not answering (" + r.error + ")";
            }
            set_wake_flag(lost);
        }
    } else {
        w_failures = 0;
    }
    return r;
}

void RemoteRenderer::Impl::run()
{
    for (;;) {
        std::deque<Command> batch;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (queue.empty()) {
                if (w_loaded && !w_finished) {
                    int64_t wait = w_next_poll - steady_ms();
                    if (wait > 0) queue_cv.wait_for(lock, std::chrono::milliseconds(wait));
                } else {
                    queue_cv.wait(lock, [this] { return !queue.empty(); });
                }
            }
            batch.swap(queue);
        }
        for (Command& c : batch)
            if (!handle(c)) return;
        if (w_loaded && !w_finished && steady_ms() >= w_next_poll) {
            poll();
            w_next_poll = steady_ms() + options.poll_ms;
        }
    }
}

bool RemoteRenderer::Impl::handle(Command& c)
{
    if (std::holds_alternative<CmdQuit>(c)) {
        if (w_loaded) action("Stop");
        return false;
    }
    if (auto* l = std::get_if<CmdLoad>(&c)) {
        do_load(*l);
        return true;
    }
    if (!w_loaded) return true;
    if (std::holds_alternative<CmdPlay>(c)) {
        w_want_playing = true;
        w_play_issued = steady_ms();
        w_finished = false;
        upnp::SoapResult r = action("Play", {{"Speed", "1"}});
        if (!r.ok) fail(r.error, true);
        else publish_position(-1, true);
        w_next_poll = steady_ms() + std::min(options.poll_ms, 500);
    } else if (std::holds_alternative<CmdPause>(c)) {
        w_want_playing = false;
        upnp::SoapResult r = action("Pause");
        if (!r.ok) fail(r.error, false);
        publish_position(-1, false);
    } else if (auto* s = std::get_if<CmdSeek>(&c)) {
        publish_position(s->ms, w_want_playing && w_saw_playing);
        if (!w_saw_playing) {
            w_pending_seek = s->ms;  // renderers reject Seek before they are playing
        } else {
            upnp::SoapResult r = action("Seek", {{"Unit", "REL_TIME"}, {"Target", upnp::format_upnp_time(s->ms)}});
            if (!r.ok) {
                w_pending_seek = s->ms;  // try again after the next poll shows PLAYING
                fail(r.error, false);
            } else {
                w_pending_seek.reset();
            }
        }
        w_next_poll = steady_ms() + std::min(options.poll_ms, 500);
    }
    return true;
}

void RemoteRenderer::Impl::do_load(const CmdLoad& c)
{
    w_generation = generation;
    w_loaded = true;
    w_want_playing = !c.paused;
    w_saw_playing = false;
    w_finished = false;
    w_pending_seek.reset();
    w_play_issued = steady_ms();

    auto index = server.index_of(c.path);
    if (!index) {
        fail(c.path + " is not in the library served to " + target.name, true);
        return;
    }
    std::string url = server.url_for(*index, server_host);
    TrackInfo info = resolve_metadata(c.path, musicdir);
    std::string didl = upnp::didl_lite_item(info.title, info.artist, info.album, url);

    action("Stop");  // harmless if nothing is playing; some renderers insist
    upnp::SoapResult r = action("SetAVTransportURI", {{"CurrentURI", url}, {"CurrentURIMetaData", didl}});
    if (!r.ok) {
        fail(r.error, true);
        return;
    }
    if (!c.paused) {
        r = action("Play", {{"Speed", "1"}});
        if (!r.ok) {
            fail(r.error, true);
            return;
        }
    }
    w_next_poll = steady_ms() + std::min(options.poll_ms, 500);
}

void RemoteRenderer::Impl::poll()
{
    upnp::SoapResult t = action("GetTransportInfo");
    upnp::SoapResult p = action("GetPositionInfo");
    if (!t.ok && !p.ok) return;  // counted towards lost() in action()

    std::string state = t.ok ? t.arg("CurrentTransportState") : "";
    int64_t rel = p.ok ? upnp::parse_upnp_time(p.arg("RelTime")) : -1;
    int64_t dur = p.ok ? upnp::parse_upnp_time(p.arg("TrackDuration")) : -1;
    if (dur > 0) {
        std::lock_guard<std::mutex> lock(status_mutex);
        if (w_generation == generation) duration = dur;
    }
    int64_t last_known;
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        last_known = interpolating ? base_ms + steady_ms() - base_time : base_ms;
    }

    if (state == "PLAYING") {
        w_saw_playing = true;
        publish_position(rel, true);
        if (w_pending_seek) {
            int64_t ms = *w_pending_seek;
            w_pending_seek.reset();
            upnp::SoapResult r = action("Seek", {{"Unit", "REL_TIME"}, {"Target", upnp::format_upnp_time(ms)}});
            if (r.ok) publish_position(ms, true);
        }
        if (!w_want_playing) {
            // We asked for a pause that did not take; ask again.
            upnp::SoapResult r = action("Pause");
            if (r.ok) publish_position(-1, false);
        }
    } else if (state == "PAUSED_PLAYBACK") {
        w_saw_playing = true;
        publish_position(rel, false);
    } else if (state == "TRANSITIONING" || state.empty()) {
        // Keep whatever we had.
    } else {  // STOPPED, NO_MEDIA_PRESENT, anything else
        if (w_saw_playing && w_want_playing) {
            int64_t pos = rel > 0 ? rel : last_known;
            int64_t d;
            {
                std::lock_guard<std::mutex> lock(status_mutex);
                d = duration;
            }
            if (d <= 0 || pos >= d - kEndSlackMs) {
                publish_position(d > 0 ? d : pos, false);
                w_finished = true;
                if (w_generation == generation) set_wake_flag(ended);
            } else {
                // Stopped from the renderer's own controls: hold here rather
                // than start the next track against the listener's wishes.
                publish_position(pos, false);
                w_want_playing = false;
            }
        } else if (!w_saw_playing && w_want_playing && steady_ms() - w_play_issued > options.start_grace_ms) {
            fail(target.name + " did not start playing", true);
        }
    }
}

} // namespace pyjuke
