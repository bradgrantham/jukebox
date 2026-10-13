// audio_control.h - the abstract audio interface the Player drives.
//
// AudioEngine (audio.h) is the real implementation; tests supply a fake.
// Kept in its own header so player.h does not drag in threads or libao.

#ifndef PYJUKE_AUDIO_CONTROL_H
#define PYJUKE_AUDIO_CONTROL_H

#include <cstdint>
#include <string>

namespace pyjuke {

class AudioControl {
public:
    virtual ~AudioControl() = default;

    // Replace whatever is loaded with `path`. If start_paused, the track is
    // opened and positioned at 0 but does not start sounding until play().
    // Clears any pending ended/error flag belonging to the previous track.
    virtual void load(const std::string& path, bool start_paused) = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    // Seek within the current track. Values < 0 are treated as 0; values past
    // the end land at the end (which then reports end-of-track).
    virtual void seek_ms(int64_t ms) = 0;

    // Current position / total duration of the loaded track, in
    // milliseconds, or -1 when unknown (nothing loaded, still opening, error).
    virtual int64_t position_ms() const = 0;
    virtual int64_t duration_ms() const = 0;

    // Poll-and-clear: true once after the current track played to its end.
    virtual bool take_track_ended() = 0;
    // Poll-and-clear: true once after the current track failed to open or
    // decode (or the output device could not be opened).
    virtual bool take_track_error() = 0;

    // Output level, 0..100. For a renderer this is the device's own volume
    // (what its remote control changes); locally it is a gain applied to
    // the decoded audio. volume() is -1 while unknown or unsupported.
    virtual void set_volume(int percent) { (void)percent; }
    virtual int volume() const { return -1; }
};

} // namespace pyjuke

#endif
