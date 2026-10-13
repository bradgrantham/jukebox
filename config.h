// config.h - ~/.pyjuke/config: plain "key = value" lines.
//
// Blank lines and lines whose first non-blank character is '#' are ignored.
// Whitespace around keys and values is trimmed; a value may be wrapped in
// double quotes. A leading "~/" in musicdir/font expands to $HOME. Unknown
// keys and unparsable values produce a warning (with file name and line
// number) and are otherwise ignored, leaving the default in place.
//
// Keys: musicdir, fullscreen, width, height, font, font_size,
//       seek_wait_millis, seek_repeat_millis, seek_delta_millis,
//       reverse_skip_millis, status_repeat_millis, clock_repeat_millis,
//       outputs, stream_port, default_output, local_volume.

#ifndef PYJUKE_CONFIG_H
#define PYJUKE_CONFIG_H

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace pyjuke {

struct Config {
    std::string musicdir;          // default "$HOME/music"
    bool fullscreen = false;
    int width = 800;               // initial window size (design space is 800x480)
    int height = 480;
    std::string font;              // empty: the graphics side picks a platform default
    int font_size = 36;            // in 800x480 design-space pixels

    int64_t seek_wait_millis = 500;
    int64_t seek_repeat_millis = 100;
    int64_t seek_delta_millis = 2000;
    int64_t reverse_skip_millis = 4000;
    int64_t status_repeat_millis = 200;  // position display refresh while playing
    int64_t clock_repeat_millis = 900;   // clock display refresh

    // Network outputs (UPnP renderers on the LAN) in the output picker.
    bool outputs = true;
    int stream_port = 0;                 // port the library is served on; 0 = any free port
    std::string default_output;          // renderer name to switch to when it appears (empty: play locally)
    // What the fader controls for "This computer": "system" (the computer's
    // output volume, falling back to "app" where that is unavailable) or
    // "app" (a gain on pyjuke's own audio only).
    std::string local_volume = "system";
};

// Defaults, with musicdir = $HOME/music (or "./music" if HOME is unset).
Config default_config();

// $HOME/.pyjuke/config (empty path if HOME is unset).
std::filesystem::path default_config_path();

// Parse `in` on top of `base`. Warnings go to `warn`, prefixed with
// `source_name` and the line number. Never throws on bad input.
Config parse_config(std::istream& in, const Config& base, std::ostream& warn,
                    const std::string& source_name = "config");

// default_config() overlaid with the file at `path` if it exists (a missing
// file is not an error; an unreadable one is warned about). Warnings go to
// std::cerr.
Config load_config(const std::filesystem::path& path = default_config_path());

} // namespace pyjuke

#endif
