// local_volume.h - the computer's own output volume, so the fader means the
// same thing for "This computer" as it does for a renderer: the device's
// level, shared with its other controls (the keyboard's volume keys).
//
// Backends: CoreAudio on macOS (the default output device's main volume),
// the ALSA "Master" mixer on Linux (which is PulseAudio's or PipeWire's sink
// volume on desktops that route ALSA through them). open() returns null
// where neither is built in or the mixer cannot be reached; AudioEngine
// then falls back to its own software gain.

#ifndef PYJUKE_LOCAL_VOLUME_H
#define PYJUKE_LOCAL_VOLUME_H

#include <memory>
#include <string>

namespace pyjuke {

class LocalVolume {
public:
    virtual ~LocalVolume() = default;

    // 0..100, or -1 if it cannot be read right now.
    virtual int volume() = 0;
    // Clamped to 0..100. False if the device refused.
    virtual bool set_volume(int percent) = 0;
    // "CoreAudio", "ALSA Master", ... for messages.
    virtual std::string name() const = 0;

    // The platform backend, or null when none is available.
    static std::unique_ptr<LocalVolume> open();
};

} // namespace pyjuke

#endif
