// player.h - jukebox logic: navigation modes, shuffle, history, seeking.
//
// Port of the Player logic in pyjuke.py.local, with no GUI, clock or audio
// library dependencies. The caller:
//   * owns an AudioControl (AudioEngine, or a fake in tests) and the track list,
//   * passes the current time in milliseconds ("now", any monotonic origin)
//     to every entry point that may start a timer,
//   * calls tick(now) whenever next_deadline() passes and whenever the audio
//     wake callback fires (end-of-track / error), and redraws if it returns true.
//
// All methods must be called from the same thread.

#ifndef PYJUKE_PLAYER_H
#define PYJUKE_PLAYER_H

#include "audio_control.h"
#include "library.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace pyjuke {

// Navigation modes, in ChangeMode() cycle order.
enum class Mode { Shuffle, Warp, Fly, Jump, Step };
// The player's intended state (audio may lag behind it briefly).
enum class PlayState { Stopped, Playing, Paused };
enum class SeekDir { Backward, Forward };

// Timing constants (defaults are the pyjuke.py.local values).
struct PlayerTiming {
    int64_t seek_wait_ms = 500;      // hold a seek key this long before seeking starts
    int64_t seek_repeat_ms = 100;    // then seek again this often
    int64_t seek_delta_ms = 2000;    // by this much (backward: delta + repeat, as before)
    int64_t reverse_skip_ms = 4000;  // prev past this position restarts the track
};

// "Random", "Skip by 1000", "Skip by 100", "Skip by 10", "Step".
const char* mode_display_name(Mode m);
// Step size of a mode (Warp 1000, Fly 100, Jump 10, Step 1; Shuffle 0).
int mode_step(Mode m);

// Port of convert_ns: "MM:SS" below an hour, "H:MM:SS" from an hour up,
// "--:--" when ms < 0 (unknown). Truncates to whole seconds.
std::string format_time(int64_t ms);

class Player {
public:
    using MetadataFn = std::function<TrackInfo(const std::filesystem::path&)>;

    // `tracks` is the scan result (index = on-screen track number).
    // `metadata` resolves a track's display info when it becomes current;
    // if empty, resolve_metadata(track, musicdir) is used.
    // `seed` seeds the shuffle RNG (pass a fixed value in tests).
    Player(AudioControl& audio, std::vector<std::filesystem::path> tracks,
           std::filesystem::path musicdir, PlayerTiming timing, uint64_t seed,
           MetadataFn metadata = {});

    // Choose the first track by the current mode (shuffle at startup, as the
    // original did) and start playing it. Does nothing with no tracks.
    void start(int64_t now);

    // ---- input entry points ----
    // Next track by the current mode (in shuffle: forward through history,
    // then a new shuffle draw).
    void next(int64_t now);
    // PrevFile2: restart the current track if it has played for more than
    // reverse_skip_ms, else the previous track (shuffle: back in history;
    // at the start of history the current track restarts).
    void prev(int64_t now);
    // Stopped -> Playing (reloads the current track), Playing <-> Paused.
    void toggle_play();
    // Shuffle -> Warp -> Fly -> Jump -> Step -> Shuffle.
    void change_mode();

    // Keyboard next/prev keys (key-down / key-up; do not forward auto-repeat).
    // Held longer than seek_wait_ms: seeks every seek_repeat_ms until release.
    // Released before that: next() / prev().
    void seek_key_press(SeekDir dir, int64_t now);
    void seek_key_release(SeekDir dir, int64_t now);
    // On-screen << / >> buttons: seek immediately on press and repeat until
    // release; release never changes track.
    void seek_button_press(SeekDir dir, int64_t now);
    void seek_button_release(SeekDir dir, int64_t now);
    // Scrollbar: seek to thousandths/1000 of the duration (clamped 0..1000).
    void scroll_seek(int thousandths);

    // Run due timers and handle end-of-track (advance as next() does) and
    // track errors (skip to the next track; stop once every track in a row
    // has failed, or when skipping cannot reach a different track).
    // Returns true if anything the UI shows may have changed.
    bool tick(int64_t now);
    // Earliest pending timer deadline, if any. (Display refresh timers such
    // as the 200 ms position update and the clock are the UI's business.)
    std::optional<int64_t> next_deadline() const;

    // ---- queries for the UI ----
    size_t track_count() const { return tracks_.size(); }
    bool has_track() const { return current_ >= 0; }
    // 0-based index of the current track (0 when there is none).
    size_t current_index() const { return current_ >= 0 ? static_cast<size_t>(current_) : 0; }
    const std::filesystem::path& current_path() const;
    const TrackInfo& info() const { return info_; }
    // Bumped every time a track (re)starts; the UI can compare to refresh.
    uint64_t track_serial() const { return serial_; }

    Mode mode() const { return mode_; }
    const char* mode_name() const { return mode_display_name(mode_); }
    PlayState state() const { return state_; }
    // "Stopped", "" (playing) or "Paused", as state_prettynames.
    const char* state_name() const;
    // True if playback stopped because consecutive tracks all failed.
    bool stopped_on_errors() const { return stopped_on_errors_; }

    int64_t position_ms() const { return audio_.position_ms(); }
    int64_t duration_ms() const { return audio_.duration_ms(); }
    // Scrollbar thumb position in thousandths, or -1 when unknown.
    int scroll_value() const;

    // ---- introspection for tests ----
    const std::vector<size_t>& history() const { return history_; }
    size_t history_cursor() const { return cursor_; }
    bool seeking() const { return seeking_; }

private:
    void start_track(size_t index, bool record_history);
    size_t draw_shuffle();
    void advance();
    void do_seek_step(int64_t now);
    void cancel_seek();

    AudioControl& audio_;
    std::vector<std::filesystem::path> tracks_;
    std::filesystem::path musicdir_;
    PlayerTiming timing_;
    MetadataFn metadata_;
    std::mt19937_64 rng_;

    Mode mode_ = Mode::Shuffle;
    PlayState state_ = PlayState::Stopped;
    long long current_ = -1;
    TrackInfo info_;
    uint64_t serial_ = 0;

    std::vector<size_t> perm_;   // current shuffle pass
    size_t perm_pos_ = 0;        // next unplayed slot in perm_
    std::vector<size_t> history_;
    size_t cursor_ = 0;          // index into history_ of the current track

    // Hold-to-seek state, as seek_wait / seeking in the original.
    bool seek_wait_ = false;
    bool seeking_ = false;
    SeekDir seek_dir_ = SeekDir::Forward;
    std::optional<int64_t> seek_deadline_;

    size_t consecutive_errors_ = 0;
    bool stopped_on_errors_ = false;
};

} // namespace pyjuke

#endif
