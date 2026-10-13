// audio_switch.h - an AudioControl that forwards to whichever output is
// current: the local AudioEngine or a RemoteRenderer. The Player holds a
// reference to the switch and never learns which one is behind it.
//
// Changing the target does not move playback by itself; the caller reloads
// the current track on the new target (Player::reload_current).

#ifndef PYJUKE_AUDIO_SWITCH_H
#define PYJUKE_AUDIO_SWITCH_H

#include "audio_control.h"

namespace pyjuke {

class AudioSwitch final : public AudioControl {
public:
    explicit AudioSwitch(AudioControl& initial) : target_(&initial) {}

    AudioControl& target() const { return *target_; }
    void set_target(AudioControl& target) { target_ = &target; }

    void load(const std::string& path, bool start_paused) override { target_->load(path, start_paused); }
    void play() override { target_->play(); }
    void pause() override { target_->pause(); }
    void seek_ms(int64_t ms) override { target_->seek_ms(ms); }
    int64_t position_ms() const override { return target_->position_ms(); }
    int64_t duration_ms() const override { return target_->duration_ms(); }
    bool take_track_ended() override { return target_->take_track_ended(); }
    bool take_track_error() override { return target_->take_track_error(); }
    void set_volume(int percent) override { target_->set_volume(percent); }
    int volume() const override { return target_->volume(); }

private:
    AudioControl* target_;
};

} // namespace pyjuke

#endif
