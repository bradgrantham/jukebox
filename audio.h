// audio.h - MP3 playback on a dedicated thread (minimp3 decode, libao output).
//
// One background thread owns the decoder (mp3dec_ex_t) and the libao device.
// All public methods are called from one controlling thread (normally the
// main/UI thread); they enqueue a command and return immediately. Status is
// published through atomics and read without locking.
//
// End-of-track and error are latched flags: the controlling thread polls them
// with take_track_ended() / take_track_error() (which clear them). Whenever a
// flag is set the wake callback given to the constructor is invoked from the
// audio thread (pass e.g. glfwPostEmptyEvent so a blocked event loop wakes).
// A flag belonging to a track that has since been replaced by load() is never
// reported.
//
// The libao device is opened lazily on the first track and reopened only when
// a track's sample rate or channel count differs from the open device.

#ifndef PYJUKE_AUDIO_H
#define PYJUKE_AUDIO_H

#include "audio_control.h"
#include "local_volume.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pyjuke {

enum class AudioState : int {
    Stopped = 0,  // nothing loaded, opening, finished, or failed
    Playing = 1,
    Paused  = 2,
};

// Multiply 16-bit samples by `gain` with rounding and clipping (gain 1 is a no-op).
void scale_samples(int16_t* samples, size_t count, float gain);

class AudioEngine final : public AudioControl {
public:
    using WakeFn = std::function<void()>;

    // Starts the audio thread. `wake` may be empty. It is called from the
    // audio thread, so it must be thread-safe (glfwPostEmptyEvent is).
    explicit AudioEngine(WakeFn wake = {});
    // Stops playback, closes the device and joins the thread.
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // AudioControl
    void load(const std::string& path, bool start_paused) override;
    void play() override;
    void pause() override;
    void seek_ms(int64_t ms) override;
    int64_t position_ms() const override;
    int64_t duration_ms() const override;
    bool take_track_ended() override;
    bool take_track_error() override;
    // Without a system backend: a software gain on the decoded samples
    // (volume squared, so the slider feels even), starting at 100. With one
    // (use_system_volume), the computer's output volume, and the gain stays
    // at 1. Opted into explicitly so tests never touch the machine's volume.
    void set_volume(int percent) override;
    int volume() const override;
    void use_system_volume(std::unique_ptr<LocalVolume> backend);
    // The backend's name, or empty when the software gain is in use.
    std::string volume_backend() const;

    AudioState state() const;
    // Human-readable description of the most recent error (empty if none).
    std::string last_error() const;
    // Sample rate / channels of the current track (0 when none).
    int sample_rate() const;
    int channels() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pyjuke

#endif
