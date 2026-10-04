// config.cpp - see config.h.

#include "config.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <system_error>

namespace pyjuke {

namespace {

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string home_dir()
{
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : std::string();
}

std::string expand_tilde(const std::string& v)
{
    if (v == "~") return home_dir();
    if (v.rfind("~/", 0) == 0) {
        std::string h = home_dir();
        if (!h.empty()) return h + v.substr(1);
    }
    return v;
}

bool parse_bool(const std::string& v, bool& out)
{
    std::string l = lower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on") { out = true; return true; }
    if (l == "0" || l == "false" || l == "no" || l == "off") { out = false; return true; }
    return false;
}

template <typename T>
bool parse_int(const std::string& v, T& out, T min, T max)
{
    T x{};
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
    if (ec != std::errc() || p != v.data() + v.size() || x < min || x > max) return false;
    out = x;
    return true;
}

} // namespace

Config default_config()
{
    Config c;
    std::string h = home_dir();
    c.musicdir = h.empty() ? std::string("music") : h + "/music";
    return c;
}

std::filesystem::path default_config_path()
{
    std::string h = home_dir();
    if (h.empty()) return {};
    return std::filesystem::path(h) / ".pyjuke" / "config";
}

Config parse_config(std::istream& in, const Config& base, std::ostream& warn, const std::string& source_name)
{
    Config c = base;
    std::string line;
    int lineno = 0;
    auto warning = [&](const std::string& msg) { warn << source_name << ":" << lineno << ": " << msg << "\n"; };

    while (std::getline(in, line)) {
        ++lineno;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            warning("expected 'key = value', ignoring line");
            continue;
        }
        std::string key = lower(trim(t.substr(0, eq)));
        std::string val = trim(t.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);

        auto bad = [&] { warning("invalid value '" + val + "' for " + key + ", ignoring"); };
        auto millis = [&](int64_t& field) {
            if (!parse_int<int64_t>(val, field, 1, 3600 * 1000)) bad();
        };

        if (key == "musicdir") {
            if (val.empty()) bad();
            else c.musicdir = expand_tilde(val);
        } else if (key == "font") {
            c.font = expand_tilde(val);
        } else if (key == "fullscreen") {
            if (!parse_bool(val, c.fullscreen)) bad();
        } else if (key == "width") {
            if (!parse_int(val, c.width, 1, 65535)) bad();
        } else if (key == "height") {
            if (!parse_int(val, c.height, 1, 65535)) bad();
        } else if (key == "font_size") {
            if (!parse_int(val, c.font_size, 1, 1000)) bad();
        } else if (key == "seek_wait_millis") {
            millis(c.seek_wait_millis);
        } else if (key == "seek_repeat_millis") {
            millis(c.seek_repeat_millis);
        } else if (key == "seek_delta_millis") {
            millis(c.seek_delta_millis);
        } else if (key == "reverse_skip_millis") {
            millis(c.reverse_skip_millis);
        } else if (key == "status_repeat_millis") {
            millis(c.status_repeat_millis);
        } else if (key == "clock_repeat_millis") {
            millis(c.clock_repeat_millis);
        } else {
            warning("unknown key '" + key + "', ignoring");
        }
    }
    return c;
}

Config load_config(const std::filesystem::path& path)
{
    Config c = default_config();
    if (path.empty()) return c;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return c;
    std::ifstream in(path);
    if (!in) {
        std::cerr << path.string() << ": cannot read, using defaults\n";
        return c;
    }
    return parse_config(in, c, std::cerr, path.string());
}

} // namespace pyjuke
