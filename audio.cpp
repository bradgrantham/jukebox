// audio.cpp - see audio.h.

#include "audio.h"

#include "minimp3_ex.h"

#include <ao/ao.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace pyjuke {

namespace {

// Samples per channel decoded and handed to libao per iteration. 1024 frames
// is ~23 ms at 44.1 kHz, so queued commands are seen at least that often.
constexpr size_t kChunkFrames = 1024;

// Requested output buffer for the macOS driver; libao's own default is
// larger, which makes pause and seek audibly sluggish.
constexpr const char* kMacBufferTimeMs = "150";

struct Command {
    enum Kind { Load, Play, Pause, Seek, Quit } kind;
    std::string path;
    bool start_paused = false;
    int64_t ms = 0;
    uint64_t gen = 0;
};

} // namespace

struct AudioEngine::Impl {
    WakeFn wake;

    // ---- shared, guarded by mtx ----
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Command> queue;
    uint64_t requested_gen = 0;   // bumped by every load()
    std::string error_text;

    // ---- shared, lock-free reads ----
    std::atomic<int64_t> pos{-1};
    std::atomic<int64_t> dur{-1};
    std::atomic<int> state{static_cast<int>(AudioState::Stopped)};
    std::atomic<int> hz{0};
    std::atomic<int> ch{0};
    std::atomic<bool> ended{false};
    std::atomic<bool> error{false};

    // ---- audio thread only ----
    std::unique_ptr<mp3dec_ex_t> dec;
    bool dec_open = false;
    bool playing = false;         // decoding/outputting (vs paused/finished)
    uint64_t cur_gen = 0;         // generation of the loaded track
    ao_device* dev = nullptr;
    int dev_hz = 0, dev_ch = 0;
    std::vector<mp3d_sample_t> buf;

    std::thread thread;

    void run();
    void do_load(const Command& c);
    void do_seek(int64_t ms);
    void play_chunk();
    void close_decoder();
    bool ensure_device(int rate, int channels);
    void finish(bool is_error, const std::string& why);
    void set_state(AudioState s) { state.store(static_cast<int>(s)); }
    int64_t frames_to_ms(uint64_t frames) const;
    void publish_position();
};

int64_t AudioEngine::Impl::frames_to_ms(uint64_t frames) const
{
    int rate = dec_open ? dec->info.hz : 0;
    if (rate <= 0) return -1;
    return static_cast<int64_t>(frames * 1000 / static_cast<uint64_t>(rate));
}

void AudioEngine::Impl::publish_position()
{
    if (!dec_open || dec->info.channels <= 0) return;
    int64_t p = frames_to_ms(dec->cur_sample / static_cast<uint64_t>(dec->info.channels));
    std::lock_guard lk(mtx);
    // If commands are pending (a seek or load the caller already reflected
    // optimistically), don't overwrite the caller's value with a stale one.
    if (queue.empty() && cur_gen == requested_gen) pos.store(p);
}

void AudioEngine::Impl::close_decoder()
{
    if (dec_open) {
        mp3dec_ex_close(dec.get());
        dec_open = false;
    }
}

bool AudioEngine::Impl::ensure_device(int rate, int channels)
{
    if (dev && dev_hz == rate && dev_ch == channels) return true;
    if (dev) {
        ao_close(dev);
        dev = nullptr;
    }
    ao_sample_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.bits = 16;
    fmt.channels = channels;
    fmt.rate = rate;
    fmt.byte_format = AO_FMT_NATIVE;

    int driver = ao_default_driver_id();
    if (driver < 0) return false;
    ao_option* opts = nullptr;
    ao_info* info = ao_driver_info(driver);
    if (info && info->short_name && std::strcmp(info->short_name, "macosx") == 0)
        ao_append_option(&opts, "buffer_time", kMacBufferTimeMs);
    dev = ao_open_live(driver, &fmt, opts);
    ao_free_options(opts);
    if (!dev) return false;
    dev_hz = rate;
    dev_ch = channels;
    return true;
}

void AudioEngine::Impl::finish(bool is_error, const std::string& why)
{
    playing = false;
    set_state(AudioState::Stopped);
    bool notify = false;
    {
        std::lock_guard lk(mtx);
        if (cur_gen == requested_gen) {
            if (is_error) {
                error_text = why;
                error.store(true);
                pos.store(-1);
                dur.store(-1);
            } else {
                ended.store(true);
            }
            notify = true;
        }
    }
    if (notify && wake) wake();
}

void AudioEngine::Impl::do_load(const Command& c)
{
    close_decoder();
    playing = false;
    cur_gen = c.gen;
    hz.store(0);
    ch.store(0);
    set_state(AudioState::Stopped);

    std::memset(dec.get(), 0, sizeof(mp3dec_ex_t));
    int r = mp3dec_ex_open(dec.get(), c.path.c_str(), MP3D_SEEK_TO_SAMPLE);
    if (r != 0) {
        finish(true, "cannot open/decode " + c.path + " (minimp3 error " + std::to_string(r) + ")");
        return;
    }
    dec_open = true;
    if (dec->info.hz <= 0 || dec->info.channels <= 0 || dec->info.channels > 2 || dec->samples == 0) {
        close_decoder();
        finish(true, "no MPEG audio frames in " + c.path);
        return;
    }
    if (!ensure_device(dec->info.hz, dec->info.channels)) {
        close_decoder();
        finish(true, "cannot open audio output device (" + std::to_string(dec->info.hz) + " Hz, " +
                     std::to_string(dec->info.channels) + " ch)");
        return;
    }
    hz.store(dec->info.hz);
    ch.store(dec->info.channels);
    {
        std::lock_guard lk(mtx);
        if (cur_gen == requested_gen) {
            uint64_t frames = dec->samples / static_cast<uint64_t>(dec->info.channels);
            dur.store(frames_to_ms(frames));
        }
    }
    playing = !c.start_paused;
    set_state(playing ? AudioState::Playing : AudioState::Paused);
}

void AudioEngine::Impl::do_seek(int64_t ms)
{
    if (!dec_open) return;
    int channels = dec->info.channels;
    uint64_t total_frames = dec->samples / static_cast<uint64_t>(channels);
    if (ms < 0) ms = 0;
    uint64_t frame = static_cast<uint64_t>(ms) * static_cast<uint64_t>(dec->info.hz) / 1000;
    frame = std::min(frame, total_frames);
    if (mp3dec_ex_seek(dec.get(), frame * static_cast<uint64_t>(channels)) != 0) {
        close_decoder();
        finish(true, "seek failed");
        return;
    }
    publish_position();
}

void AudioEngine::Impl::play_chunk()
{
    int channels = dec->info.channels;
    size_t want = kChunkFrames * static_cast<size_t>(channels);
    if (buf.size() < want) buf.resize(want);
    size_t got = mp3dec_ex_read(dec.get(), buf.data(), want);
    if (got > 0 && dev) {
        // ao_play blocks until the device has room; that is our pacing.
        ao_play(dev, reinterpret_cast<char*>(buf.data()),
                static_cast<uint_32>(got * sizeof(mp3d_sample_t)));
    }
    publish_position();
    if (got < want) {
        if (dec->last_error != 0)
            finish(true, "decode error " + std::to_string(dec->last_error));
        else
            finish(false, {});
    }
}

void AudioEngine::Impl::run()
{
    ao_initialize();
    dec = std::make_unique<mp3dec_ex_t>();
    std::memset(dec.get(), 0, sizeof(mp3dec_ex_t));

    bool quit = false;
    while (!quit) {
        std::vector<Command> cmds;
        {
            std::unique_lock lk(mtx);
            if (!(dec_open && playing))
                cv.wait(lk, [&] { return !queue.empty(); });
            cmds.assign(queue.begin(), queue.end());
            queue.clear();
        }

        // Only the newest Load matters; anything queued before it is moot.
        size_t first = 0;
        for (size_t i = 0; i < cmds.size(); ++i) {
            if (cmds[i].kind == Command::Quit) quit = true;
            if (cmds[i].kind == Command::Load) first = i;
        }
        if (quit) break;

        for (size_t i = first; i < cmds.size(); ++i) {
            const Command& c = cmds[i];
            switch (c.kind) {
            case Command::Load:
                do_load(c);
                break;
            case Command::Play:
                if (dec_open) {
                    playing = true;
                    set_state(AudioState::Playing);
                }
                break;
            case Command::Pause:
                if (dec_open) {
                    playing = false;
                    set_state(AudioState::Paused);
                }
                break;
            case Command::Seek:
                do_seek(c.ms);
                break;
            case Command::Quit:
                break;
            }
        }

        if (dec_open && playing) play_chunk();
    }

    close_decoder();
    if (dev) ao_close(dev);
    dev = nullptr;
    ao_shutdown();
}

// ---------------------------------------------------------------------------

AudioEngine::AudioEngine(WakeFn wake) : impl_(std::make_unique<Impl>())
{
    impl_->wake = std::move(wake);
    impl_->thread = std::thread([this] { impl_->run(); });
}

AudioEngine::~AudioEngine()
{
    {
        std::lock_guard lk(impl_->mtx);
        impl_->queue.push_back(Command{Command::Quit, {}, false, 0, 0});
    }
    impl_->cv.notify_one();
    if (impl_->thread.joinable()) impl_->thread.join();
}

void AudioEngine::load(const std::string& path, bool start_paused)
{
    {
        std::lock_guard lk(impl_->mtx);
        uint64_t gen = ++impl_->requested_gen;
        impl_->ended.store(false);
        impl_->error.store(false);
        impl_->error_text.clear();
        impl_->pos.store(0);
        impl_->dur.store(-1);
        impl_->queue.push_back(Command{Command::Load, path, start_paused, 0, gen});
    }
    impl_->cv.notify_one();
}

void AudioEngine::play()
{
    {
        std::lock_guard lk(impl_->mtx);
        impl_->queue.push_back(Command{Command::Play, {}, false, 0, 0});
    }
    impl_->cv.notify_one();
}

void AudioEngine::pause()
{
    {
        std::lock_guard lk(impl_->mtx);
        impl_->queue.push_back(Command{Command::Pause, {}, false, 0, 0});
    }
    impl_->cv.notify_one();
}

void AudioEngine::seek_ms(int64_t ms)
{
    {
        std::lock_guard lk(impl_->mtx);
        if (ms < 0) ms = 0;
        int64_t d = impl_->dur.load();
        if (d >= 0 && ms > d) ms = d;
        impl_->pos.store(ms);  // optimistic, so repeated relative seeks compose
        impl_->queue.push_back(Command{Command::Seek, {}, false, ms, 0});
    }
    impl_->cv.notify_one();
}

int64_t AudioEngine::position_ms() const { return impl_->pos.load(); }
int64_t AudioEngine::duration_ms() const { return impl_->dur.load(); }
bool AudioEngine::take_track_ended() { return impl_->ended.exchange(false); }
bool AudioEngine::take_track_error() { return impl_->error.exchange(false); }
AudioState AudioEngine::state() const { return static_cast<AudioState>(impl_->state.load()); }
int AudioEngine::sample_rate() const { return impl_->hz.load(); }
int AudioEngine::channels() const { return impl_->ch.load(); }

std::string AudioEngine::last_error() const
{
    std::lock_guard lk(impl_->mtx);
    return impl_->error_text;
}

} // namespace pyjuke
