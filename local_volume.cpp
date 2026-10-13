// local_volume.cpp - see local_volume.h.

#include "local_volume.h"

#include <algorithm>
#include <cmath>

#if defined(__APPLE__) && defined(PYJUKE_HAVE_COREAUDIO)
#include <CoreAudio/CoreAudio.h>
#endif
#if defined(PYJUKE_HAVE_ALSA)
#include <alsa/asoundlib.h>
#endif

namespace pyjuke {

namespace {

#if defined(__APPLE__) && defined(PYJUKE_HAVE_COREAUDIO)

// Property selectors spelled out so this compiles against any SDK version:
// kAudioHardwareServiceDeviceProperty_VirtualMainVolume ('vmvc', formerly
// ...VirtualMasterVolume) and kAudioDevicePropertyMute ('mute').
constexpr AudioObjectPropertySelector fourcc(char a, char b, char c, char d)
{
    return (static_cast<AudioObjectPropertySelector>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<AudioObjectPropertySelector>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<AudioObjectPropertySelector>(static_cast<unsigned char>(c)) << 8) |
           static_cast<AudioObjectPropertySelector>(static_cast<unsigned char>(d));
}
constexpr AudioObjectPropertySelector kVirtualMainVolume = fourcc('v', 'm', 'v', 'c');
constexpr AudioObjectPropertySelector kMute = fourcc('m', 'u', 't', 'e');
constexpr AudioObjectPropertyElement kMainElement = 0;  // kAudioObjectPropertyElementMain / ...Master

class CoreAudioVolume final : public LocalVolume {
public:
    std::string name() const override { return "CoreAudio"; }

    int volume() override
    {
        AudioDeviceID dev = default_output();
        if (dev == kAudioObjectUnknown) return -1;
        Float32 v = 0;
        if (!get_scalar(dev, kVirtualMainVolume, kMainElement, &v)) {
            // No virtual main volume (some USB interfaces): average the channels.
            Float32 l = 0, r = 0;
            bool got_l = get_scalar(dev, kAudioDevicePropertyVolumeScalar, 1, &l);
            bool got_r = get_scalar(dev, kAudioDevicePropertyVolumeScalar, 2, &r);
            if (!got_l && !got_r) return -1;
            v = got_l && got_r ? (l + r) / 2 : (got_l ? l : r);
        }
        return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 100));
    }

    bool set_volume(int percent) override
    {
        AudioDeviceID dev = default_output();
        if (dev == kAudioObjectUnknown) return false;
        Float32 v = static_cast<Float32>(std::clamp(percent, 0, 100)) / 100.0f;
        bool ok = set_scalar(dev, kVirtualMainVolume, kMainElement, v);
        if (!ok) {
            bool l = set_scalar(dev, kAudioDevicePropertyVolumeScalar, 1, v);
            bool r = set_scalar(dev, kAudioDevicePropertyVolumeScalar, 2, v);
            ok = l || r;
        }
        if (ok && percent > 0) {
            // Turning the level up should be audible: clear a mute if there is one.
            AudioObjectPropertyAddress addr{kMute, kAudioDevicePropertyScopeOutput, kMainElement};
            if (AudioObjectHasProperty(dev, &addr)) {
                UInt32 unmuted = 0;
                AudioObjectSetPropertyData(dev, &addr, 0, nullptr, sizeof unmuted, &unmuted);
            }
        }
        return ok;
    }

private:
    static AudioDeviceID default_output()
    {
        AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kMainElement};
        AudioDeviceID dev = kAudioObjectUnknown;
        UInt32 size = sizeof dev;
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &dev) != noErr) return kAudioObjectUnknown;
        return dev;
    }
    static bool get_scalar(AudioDeviceID dev, AudioObjectPropertySelector sel, AudioObjectPropertyElement element, Float32* out)
    {
        AudioObjectPropertyAddress addr{sel, kAudioDevicePropertyScopeOutput, element};
        if (!AudioObjectHasProperty(dev, &addr)) return false;
        UInt32 size = sizeof *out;
        return AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, out) == noErr;
    }
    static bool set_scalar(AudioDeviceID dev, AudioObjectPropertySelector sel, AudioObjectPropertyElement element, Float32 v)
    {
        AudioObjectPropertyAddress addr{sel, kAudioDevicePropertyScopeOutput, element};
        Boolean settable = false;
        if (!AudioObjectHasProperty(dev, &addr)) return false;
        if (AudioObjectIsPropertySettable(dev, &addr, &settable) != noErr || !settable) return false;
        return AudioObjectSetPropertyData(dev, &addr, 0, nullptr, sizeof v, &v) == noErr;
    }
};

#endif  // CoreAudio

#if defined(PYJUKE_HAVE_ALSA)

// alsa-lib reports configuration trouble (no sound card at all, say) on
// stderr through this hook; a machine without a mixer is not an error here.
void quiet_alsa_errors(const char*, int, const char*, int, const char*, ...) {}

class AlsaVolume final : public LocalVolume {
public:
    ~AlsaVolume() override
    {
        if (mixer_) snd_mixer_close(mixer_);
    }

    static std::unique_ptr<AlsaVolume> open()
    {
        snd_lib_error_set_handler(quiet_alsa_errors);
        std::unique_ptr<AlsaVolume> a(new AlsaVolume);
        if (snd_mixer_open(&a->mixer_, 0) < 0) return nullptr;
        if (snd_mixer_attach(a->mixer_, "default") < 0 || snd_mixer_selem_register(a->mixer_, nullptr, nullptr) < 0 ||
            snd_mixer_load(a->mixer_) < 0)
            return nullptr;
        for (const char* name : {"Master", "PCM", "Speaker", "Headphone"}) {
            snd_mixer_selem_id_t* sid;
            snd_mixer_selem_id_alloca(&sid);
            snd_mixer_selem_id_set_index(sid, 0);
            snd_mixer_selem_id_set_name(sid, name);
            snd_mixer_elem_t* e = snd_mixer_find_selem(a->mixer_, sid);
            if (e != nullptr && snd_mixer_selem_has_playback_volume(e)) {
                a->elem_ = e;
                a->elem_name_ = name;
                break;
            }
        }
        if (a->elem_ == nullptr) return nullptr;
        if (snd_mixer_selem_get_playback_volume_range(a->elem_, &a->min_, &a->max_) < 0 || a->max_ <= a->min_) return nullptr;
        return a;
    }

    std::string name() const override { return "ALSA " + elem_name_; }

    int volume() override
    {
        snd_mixer_handle_events(mixer_);  // pick up changes made elsewhere
        long v = 0;
        if (snd_mixer_selem_get_playback_volume(elem_, SND_MIXER_SCHN_FRONT_LEFT, &v) < 0 &&
            snd_mixer_selem_get_playback_volume(elem_, SND_MIXER_SCHN_MONO, &v) < 0)
            return -1;
        if (snd_mixer_selem_has_playback_switch(elem_)) {
            int on = 1;
            if (snd_mixer_selem_get_playback_switch(elem_, SND_MIXER_SCHN_FRONT_LEFT, &on) == 0 && !on) return 0;
        }
        return static_cast<int>(std::lround(100.0 * static_cast<double>(v - min_) / static_cast<double>(max_ - min_)));
    }

    bool set_volume(int percent) override
    {
        percent = std::clamp(percent, 0, 100);
        long v = min_ + static_cast<long>(std::lround(static_cast<double>(max_ - min_) * percent / 100.0));
        if (snd_mixer_selem_set_playback_volume_all(elem_, v) < 0) return false;
        if (percent > 0 && snd_mixer_selem_has_playback_switch(elem_)) snd_mixer_selem_set_playback_switch_all(elem_, 1);
        return true;
    }

private:
    AlsaVolume() = default;
    snd_mixer_t* mixer_ = nullptr;
    snd_mixer_elem_t* elem_ = nullptr;
    std::string elem_name_;
    long min_ = 0, max_ = 0;
};

#endif  // ALSA

} // namespace

std::unique_ptr<LocalVolume> LocalVolume::open()
{
#if defined(__APPLE__) && defined(PYJUKE_HAVE_COREAUDIO)
    auto c = std::make_unique<CoreAudioVolume>();
    if (c->volume() >= 0) return c;
    return nullptr;
#elif defined(PYJUKE_HAVE_ALSA)
    return AlsaVolume::open();
#else
    return nullptr;
#endif
}

} // namespace pyjuke
