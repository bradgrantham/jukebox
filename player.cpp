// player.cpp - see player.h. Behaviour ported from pyjuke.py.local:
// NextFile/PrevFile/PrevFile2, Begin/Seek/Finish Forward/Backward,
// ScrollSeek, ChangeMode, TogglePlaying, convert_ns.

#include "player.h"

#include <algorithm>
#include <cstdio>

namespace fs = std::filesystem;

namespace pyjuke {

namespace {
constexpr size_t kMaxHistory = 10000;
}

const char* mode_display_name(Mode m)
{
    switch (m) {
    case Mode::Shuffle: return "Random";
    case Mode::Warp: return "Skip by 1000";
    case Mode::Fly: return "Skip by 100";
    case Mode::Jump: return "Skip by 10";
    case Mode::Step: return "Step";
    }
    return "";
}

int mode_step(Mode m)
{
    switch (m) {
    case Mode::Shuffle: return 0;
    case Mode::Warp: return 1000;
    case Mode::Fly: return 100;
    case Mode::Jump: return 10;
    case Mode::Step: return 1;
    }
    return 0;
}

std::string format_time(int64_t ms)
{
    if (ms < 0) return "--:--";
    long long t = ms / 1000;
    long long h = t / 3600, m = (t / 60) % 60, s = t % 60;
    char buf[48];
    if (h > 0) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", h, m, s);
    else std::snprintf(buf, sizeof buf, "%02lld:%02lld", m, s);
    return buf;
}

Player::Player(AudioControl& audio, std::vector<fs::path> tracks, fs::path musicdir,
               PlayerTiming timing, uint64_t seed, MetadataFn metadata)
    : audio_(audio), tracks_(std::move(tracks)), musicdir_(std::move(musicdir)), timing_(timing),
      metadata_(std::move(metadata)), rng_(seed)
{
    if (!metadata_) {
        fs::path md = musicdir_;
        metadata_ = [md](const fs::path& p) { return resolve_metadata(p, md); };
    }
}

const fs::path& Player::current_path() const
{
    static const fs::path none;
    return current_ >= 0 ? tracks_[static_cast<size_t>(current_)] : none;
}

const char* Player::state_name() const
{
    switch (state_) {
    case PlayState::Stopped: return "Stopped";
    case PlayState::Playing: return "";
    case PlayState::Paused: return "Paused";
    }
    return "";
}

int Player::scroll_value() const
{
    int64_t d = audio_.duration_ms(), p = audio_.position_ms();
    if (d <= 0 || p < 0) return -1;
    return static_cast<int>(std::clamp<int64_t>(p * 1000 / d, 0, 1000));
}

void Player::start(int64_t now)
{
    (void)now;
    if (tracks_.empty()) return;
    state_ = PlayState::Playing;
    advance();
}

void Player::start_track(size_t index, bool record_history)
{
    if (record_history) {
        if (!history_.empty()) history_.resize(cursor_ + 1);  // drop the "forward" branch
        history_.push_back(index);
        if (history_.size() > kMaxHistory)
            history_.erase(history_.begin(), history_.begin() + static_cast<long>(history_.size() - kMaxHistory));
        cursor_ = history_.size() - 1;
    }
    if (state_ == PlayState::Stopped) state_ = PlayState::Playing;
    current_ = static_cast<long long>(index);
    info_ = metadata_(tracks_[index]);
    ++serial_;
    audio_.load(tracks_[index].string(), state_ == PlayState::Paused);
}

size_t Player::draw_shuffle()
{
    const size_t n = tracks_.size();
    if (perm_.size() != n || perm_pos_ >= perm_.size()) {
        perm_.resize(n);
        for (size_t i = 0; i < n; ++i) perm_[i] = i;
        for (size_t i = n - 1; i > 0; --i) {  // Fisher-Yates
            std::uniform_int_distribution<size_t> pick(0, i);
            std::swap(perm_[i], perm_[pick(rng_)]);
        }
        // Don't repeat the track just played across a pass boundary.
        if (n > 1 && current_ >= 0 && perm_[0] == static_cast<size_t>(current_)) std::swap(perm_[0], perm_[1]);
        perm_pos_ = 0;
    }
    return perm_[perm_pos_++];
}

void Player::advance()
{
    const size_t n = tracks_.size();
    if (n == 0) return;
    if (mode_ == Mode::Shuffle) {
        if (!history_.empty() && cursor_ + 1 < history_.size()) {
            ++cursor_;
            start_track(history_[cursor_], false);
        } else {
            start_track(draw_shuffle(), true);
        }
        return;
    }
    size_t cur = current_ >= 0 ? static_cast<size_t>(current_) : 0;
    size_t idx;
    if (mode_ == Mode::Step) idx = (cur + 1) % n;  // wrap
    else idx = std::min(cur + static_cast<size_t>(mode_step(mode_)), n - 1);  // clamp
    start_track(idx, true);
}

void Player::next(int64_t now)
{
    (void)now;
    consecutive_errors_ = 0;
    stopped_on_errors_ = false;
    advance();
}

void Player::prev(int64_t now)
{
    (void)now;
    const size_t n = tracks_.size();
    if (n == 0) return;
    consecutive_errors_ = 0;
    stopped_on_errors_ = false;
    if (current_ >= 0 && state_ != PlayState::Stopped && audio_.position_ms() > timing_.reverse_skip_ms) {
        audio_.seek_ms(0);
        return;
    }
    if (mode_ == Mode::Shuffle) {
        if (cursor_ > 0 && cursor_ < history_.size()) {
            --cursor_;
            start_track(history_[cursor_], false);
        } else if (current_ >= 0) {
            start_track(static_cast<size_t>(current_), false);  // nothing earlier: restart
        } else {
            advance();
        }
        return;
    }
    size_t cur = current_ >= 0 ? static_cast<size_t>(current_) : 0;
    size_t step = static_cast<size_t>(mode_step(mode_));
    size_t idx;
    if (mode_ == Mode::Step) idx = (cur + n - 1) % n;  // wrap
    else idx = cur >= step ? cur - step : 0;          // clamp
    start_track(idx, true);
}

void Player::toggle_play()
{
    if (tracks_.empty()) return;
    switch (state_) {
    case PlayState::Stopped:
        state_ = PlayState::Playing;
        consecutive_errors_ = 0;
        stopped_on_errors_ = false;
        if (current_ >= 0) start_track(static_cast<size_t>(current_), false);
        else advance();
        break;
    case PlayState::Playing:
        state_ = PlayState::Paused;
        audio_.pause();
        break;
    case PlayState::Paused:
        state_ = PlayState::Playing;
        audio_.play();
        break;
    }
}

void Player::reload_current(int64_t position_ms)
{
    if (current_ < 0 || state_ == PlayState::Stopped) return;
    start_track(static_cast<size_t>(current_), false);
    if (position_ms > 0) audio_.seek_ms(position_ms);
}

void Player::change_mode()
{
    switch (mode_) {
    case Mode::Shuffle: mode_ = Mode::Warp; break;
    case Mode::Warp: mode_ = Mode::Fly; break;
    case Mode::Fly: mode_ = Mode::Jump; break;
    case Mode::Jump: mode_ = Mode::Step; break;
    case Mode::Step: mode_ = Mode::Shuffle; break;
    }
}

void Player::cancel_seek()
{
    seek_wait_ = false;
    seeking_ = false;
    seek_deadline_.reset();
}

void Player::do_seek_step(int64_t now)
{
    if (!seek_wait_ && !seeking_) return;
    if (seek_wait_) {
        seek_wait_ = false;
        seeking_ = true;
    }
    int64_t pos = audio_.position_ms();
    if (pos >= 0 && current_ >= 0 && state_ != PlayState::Stopped) {
        int64_t target;
        if (seek_dir_ == SeekDir::Forward) target = pos + timing_.seek_delta_ms;
        else target = std::max<int64_t>(0, pos - (timing_.seek_repeat_ms + timing_.seek_delta_ms));
        audio_.seek_ms(target);
    }
    seek_deadline_ = now + timing_.seek_repeat_ms;
}

void Player::seek_key_press(SeekDir dir, int64_t now)
{
    seek_dir_ = dir;
    seek_wait_ = true;
    seeking_ = false;
    seek_deadline_ = now + timing_.seek_wait_ms;
}

void Player::seek_key_release(SeekDir dir, int64_t now)
{
    if (dir != seek_dir_) return;  // release of a key whose press was superseded
    bool short_press = seek_wait_;
    cancel_seek();
    if (short_press) {
        if (dir == SeekDir::Forward) next(now);
        else prev(now);
    }
}

void Player::seek_button_press(SeekDir dir, int64_t now)
{
    seek_dir_ = dir;
    seek_wait_ = true;
    seeking_ = false;
    do_seek_step(now);  // immediate: clears seek_wait_, so release won't skip
}

void Player::seek_button_release(SeekDir dir, int64_t now)
{
    seek_key_release(dir, now);
}

void Player::scroll_seek(int thousandths)
{
    int64_t d = audio_.duration_ms();
    if (d <= 0 || current_ < 0) return;
    thousandths = std::clamp(thousandths, 0, 1000);
    audio_.seek_ms(d * thousandths / 1000);
}

bool Player::tick(int64_t now)
{
    bool changed = false;

    if (seek_deadline_ && now >= *seek_deadline_) {
        seek_deadline_.reset();
        do_seek_step(now);
        changed = true;
    }

    if (audio_.take_track_error()) {
        changed = true;
        if (state_ != PlayState::Stopped && !tracks_.empty()) {
            ++consecutive_errors_;
            const size_t n = tracks_.size();
            bool give_up = consecutive_errors_ >= n;
            if (!give_up && current_ >= 0) {
                // In clamping modes at the last track, "next" is the same
                // failing track again; don't retry it.
                size_t cur = static_cast<size_t>(current_);
                if (mode_ == Mode::Shuffle) give_up = (n == 1);
                else if (mode_ != Mode::Step) give_up = (cur + 1 >= n);
            }
            if (give_up) {
                state_ = PlayState::Stopped;
                stopped_on_errors_ = true;
                cancel_seek();
            } else {
                advance();
            }
        }
    }

    if (audio_.take_track_ended()) {
        changed = true;
        consecutive_errors_ = 0;
        if (state_ != PlayState::Stopped) advance();
    }

    // A track that is actually producing audio ends an error streak.
    if (consecutive_errors_ > 0 && audio_.position_ms() > 0 && audio_.duration_ms() > 0)
        consecutive_errors_ = 0;

    return changed;
}

std::optional<int64_t> Player::next_deadline() const
{
    return seek_deadline_;
}

} // namespace pyjuke
