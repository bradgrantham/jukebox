// tests/test_main.cpp - unit tests for the pyjuke core (plain asserts).

#undef NDEBUG
#include <cassert>

#include "audio_switch.h"
#include "config.h"
#include "id3.h"
#include "library.h"
#include "player.h"

void run_network_tests(const std::filesystem::path& tmp);  // tests/test_network.cpp

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace pyjuke;
using Bytes = std::vector<uint8_t>;

#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        auto _a = (a);                                                                                \
        auto _b = (b);                                                                                \
        if (!(_a == _b)) {                                                                            \
            std::ostringstream _o;                                                                    \
            _o << _a << " != " << _b;                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: %s\n", __FILE__, __LINE__, #a, #b, \
                         _o.str().c_str());                                                           \
            assert(false);                                                                            \
        }                                                                                             \
    } while (0)

static void section(const char* name) { std::printf("  %s\n", name); }

// ---------------------------------------------------------------------------
// ID3 fixture builders

static void append(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }
static void append(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }

static Bytes syncsafe(uint32_t v)
{
    return {uint8_t((v >> 21) & 0x7F), uint8_t((v >> 14) & 0x7F), uint8_t((v >> 7) & 0x7F), uint8_t(v & 0x7F)};
}
static Bytes be32b(uint32_t v) { return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }

// Valid (if wasteful) unsynchronisation: a 0x00 after every 0xFF.
static Bytes unsync(const Bytes& in)
{
    Bytes out;
    for (uint8_t c : in) {
        out.push_back(c);
        if (c == 0xFF) out.push_back(0x00);
    }
    return out;
}

static Bytes text_latin1(const std::string& s)
{
    Bytes b{0};
    append(b, s);
    return b;
}
static Bytes text_utf8(const std::string& s)
{
    Bytes b{3};
    append(b, s);
    b.push_back(0);
    return b;
}
static Bytes text_utf16(const std::u16string& s, bool bom, bool big_endian, uint8_t enc)
{
    Bytes b{enc};
    if (bom) append(b, big_endian ? Bytes{0xFE, 0xFF} : Bytes{0xFF, 0xFE});
    for (char16_t c : s) {
        if (big_endian) { b.push_back(uint8_t(c >> 8)); b.push_back(uint8_t(c & 0xFF)); }
        else { b.push_back(uint8_t(c & 0xFF)); b.push_back(uint8_t(c >> 8)); }
    }
    b.push_back(0);
    b.push_back(0);
    return b;
}

static Bytes frame(int major, const std::string& id, const Bytes& data, uint16_t flags = 0, bool plain_size = false)
{
    Bytes f;
    append(f, id);
    uint32_t n = uint32_t(data.size());
    if (major == 2) {
        append(f, Bytes{uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
    } else {
        append(f, (major == 4 && !plain_size) ? syncsafe(n) : be32b(n));
        f.push_back(uint8_t(flags >> 8));
        f.push_back(uint8_t(flags & 0xFF));
    }
    append(f, data);
    return f;
}

static Bytes tag(int major, uint8_t flags, const Bytes& body, size_t padding = 0)
{
    Bytes t{'I', 'D', '3', uint8_t(major), 0, flags};
    Bytes b = body;
    b.resize(b.size() + padding, 0);
    append(t, syncsafe(uint32_t(b.size())));
    append(t, b);
    return t;
}

static Bytes v1(const std::string& title, const std::string& artist, const std::string& album)
{
    Bytes b(128, 0);
    std::memcpy(b.data(), "TAG", 3);
    std::memcpy(b.data() + 3, title.data(), std::min<size_t>(30, title.size()));
    std::memcpy(b.data() + 33, artist.data(), std::min<size_t>(30, artist.size()));
    std::memcpy(b.data() + 63, album.data(), std::min<size_t>(30, album.size()));
    return b;
}

static Id3Tags parse(const Bytes& b) { return parse_id3v2(b.data(), b.size()); }

static const std::string kCafe = "Caf\xC3\xA9";       // "Café"
static const std::string kNote = "\xF0\x9F\x8E\xB5";  // U+1F3B5
static const std::string kRepl = "\xEF\xBF\xBD";      // U+FFFD

static void test_id3()
{
    section("id3v1");
    {
        Bytes b = v1("Title   ", "Caf\xE9", "Album");
        Id3Tags t = parse_id3v1(b.data(), b.size());
        CHECK_EQ(t.title, std::string("Title"));
        CHECK_EQ(t.artist, kCafe);
        CHECK_EQ(t.album, std::string("Album"));
        Bytes bad = b;
        bad[0] = 'X';
        assert(parse_id3v1(bad.data(), bad.size()).empty());
        assert(parse_id3v1(b.data(), 100).empty());
        b[125] = 0;  // v1.1 track number
        b[126] = 7;
        CHECK_EQ(parse_id3v1(b.data(), b.size()).album, std::string("Album"));
    }

    section("id3v2.2: latin-1, utf-16 LE/BE BOM, compression flag");
    {
        Bytes body;
        append(body, frame(2, "TT2", text_latin1("Caf\xE9")));
        append(body, frame(2, "TP1", text_utf16(u"Artist", true, false, 1)));
        append(body, frame(2, "TAL", text_utf16(u"Album", true, true, 1)));
        Id3Tags t = parse(tag(2, 0, body, 32));
        CHECK_EQ(t.title, kCafe);
        CHECK_EQ(t.artist, std::string("Artist"));
        CHECK_EQ(t.album, std::string("Album"));
        assert(parse(tag(2, 0x40, body)).empty());
    }

    section("id3v2.3: encodings, surrogates, first-of-duplicates, grouping, compressed frame");
    {
        Bytes body;
        append(body, frame(3, "TXXX", text_latin1("ignored")));
        append(body, frame(3, "TIT2", text_utf16(u"Café \U0001F3B5", true, false, 1)));
        append(body, frame(3, "TIT2", text_latin1("second")));
        append(body, frame(3, "TPE1", Bytes{0} /*placeholder replaced below*/, 0x0080));  // compressed: skipped
        Bytes grouped{0x42};
        append(grouped, text_utf16(u"BigEnd", true, true, 1));
        append(body, frame(3, "TPE1", grouped, 0x0020));
        append(body, frame(3, "TALB", text_latin1(std::string("Alb\0junk", 8))));
        Id3Tags t = parse(tag(3, 0, body, 100));
        CHECK_EQ(t.title, kCafe + " " + kNote);
        CHECK_EQ(t.artist, std::string("BigEnd"));
        CHECK_EQ(t.album, std::string("Alb"));
    }

    section("id3v2.3: extended header + tag-level unsynchronisation");
    {
        Bytes body;
        append(body, be32b(6));                          // ext header size (excl. itself)
        append(body, Bytes{0, 0, 0, 0, 0, 0});
        append(body, frame(3, "TIT2", text_latin1("a\xFF" "b")));  // 0xFF = 'ÿ'
        append(body, frame(3, "TPE1", text_utf16(u"ÿÿ", true, false, 1)));  // FF FE FF 00 FF 00
        Id3Tags t = parse(tag(3, 0x80 | 0x40, unsync(body)));
        CHECK_EQ(t.title, std::string("a\xC3\xBF" "b"));
        CHECK_EQ(t.artist, std::string("\xC3\xBF\xC3\xBF"));
    }

    section("id3v2.4: utf-8, utf-16BE no BOM, multi-value, ext header");
    {
        Bytes body;
        append(body, syncsafe(6));  // ext header size incl. itself
        append(body, Bytes{1, 0});
        append(body, frame(4, "TIT2", text_utf8(kCafe + kNote)));
        append(body, frame(4, "TPE1", text_utf16(u"Bigé", false, true, 2)));
        Bytes multi{0};
        append(multi, std::string("One\0Two", 7));
        append(body, frame(4, "TALB", multi));
        Id3Tags t = parse(tag(4, 0x40, body, 10));
        CHECK_EQ(t.title, kCafe + kNote);
        CHECK_EQ(t.artist, std::string("Big") + "\xC3\xA9");
        CHECK_EQ(t.album, std::string("One"));
    }

    section("id3v2.4: per-frame unsync + data length indicator, header unsync flag, plain sizes");
    {
        Bytes data = text_latin1("x\xFFy");
        Bytes fd = syncsafe(uint32_t(data.size()));  // data length indicator
        append(fd, unsync(data));
        Bytes body;
        append(body, frame(4, "TIT2", fd, 0x0002 | 0x0001));
        Id3Tags t = parse(tag(4, 0, body));
        CHECK_EQ(t.title, std::string("x\xC3\xBFy"));

        // Header-level unsync flag in v2.4 means each frame's data is unsynced.
        Bytes body2;
        append(body2, frame(4, "TPE1", unsync(text_latin1("p\xFFq"))));
        CHECK_EQ(parse(tag(4, 0x80, body2)).artist, std::string("p\xC3\xBFq"));

        // iTunes-style v2.4 with plain big-endian frame sizes >= 128.
        std::string longtitle(200, 'L');
        Bytes body3;
        append(body3, frame(4, "TIT2", text_latin1(longtitle), 0, true));
        append(body3, frame(4, "TPE1", text_latin1("after"), 0, true));
        Id3Tags t3 = parse(tag(4, 0, body3));
        CHECK_EQ(t3.title, longtitle);
        CHECK_EQ(t3.artist, std::string("after"));
    }

    section("utf-8 sanitizing and utf-16 lone surrogates");
    {
        CHECK_EQ(sanitize_utf8("ok\xC3"), std::string("ok") + kRepl);
        CHECK_EQ(sanitize_utf8("\xC0\x80"), kRepl + kRepl);  // overlong
        CHECK_EQ(sanitize_utf8("\xED\xA0\x80"), kRepl + kRepl + kRepl);  // encoded surrogate
        Bytes body;
        append(body, frame(3, "TIT2", text_utf16(u"a\xD800" "b", true, false, 1)));
        CHECK_EQ(parse(tag(3, 0, body)).title, "a" + kRepl + "b");
        Bytes body2;
        append(body2, frame(4, "TIT2", text_utf8("bad\xFF")));
        CHECK_EQ(parse(tag(4, 0, body2)).title, "bad" + kRepl);
    }

    section("id3v2 malformed: truncation at every length, bad sizes, random garbage");
    {
        Bytes body;
        append(body, frame(3, "TIT2", text_utf16(u"Title", true, false, 1)));
        append(body, frame(3, "TPE1", text_latin1("Artist")));
        append(body, frame(3, "TALB", text_latin1("Album")));
        Bytes full = tag(3, 0, body);
        for (size_t n = 0; n <= full.size(); ++n) {
            Id3Tags t = parse_id3v2(full.data(), n);  // never reads past n
            if (n == full.size()) CHECK_EQ(t.album, std::string("Album"));
        }
        // Frame claims to be bigger than the tag.
        Bytes huge = full;
        huge[10 + 4] = 0x7F;
        Id3Tags th = parse(huge);
        assert(th.title.empty());
        // Non-syncsafe tag size, bad version, bad magic.
        Bytes ns = full;
        ns[6] = 0x80;
        assert(parse(ns).empty());
        Bytes bv = full;
        bv[3] = 9;
        assert(parse(bv).empty());
        assert(parse_id3v2(nullptr, 0).empty());
        // Extended header claiming more than the tag.
        Bytes eh = tag(3, 0x40, Bytes{0x7F, 0xFF, 0xFF, 0xFF, 1, 2});
        assert(parse(eh).empty());
        // Random garbage behind valid-looking headers.
        std::mt19937 rng(1234);
        for (int iter = 0; iter < 5000; ++iter) {
            size_t len = rng() % 300;
            Bytes g(10 + len);
            for (auto& c : g) c = uint8_t(rng());
            g[0] = 'I'; g[1] = 'D'; g[2] = '3';
            g[3] = uint8_t(2 + rng() % 3);
            g[4] = 0;
            for (int k = 6; k < 10; ++k) g[size_t(k)] &= 0x7F;
            if (iter % 2) {  // make the body look like frames more often
                Bytes s = syncsafe(uint32_t(len));
                std::copy(s.begin(), s.end(), g.begin() + 6);
                if (len > 4) { g[10] = 'T'; g[11] = 'I'; g[12] = 'T'; g[13] = '2'; }
            }
            Id3Tags t = parse(g);
            // whatever comes out must be valid UTF-8
            CHECK_EQ(sanitize_utf8(t.title), t.title);
            CHECK_EQ(sanitize_utf8(t.artist), t.artist);
            CHECK_EQ(sanitize_utf8(t.album), t.album);
        }
    }
}

// ---------------------------------------------------------------------------
// Temporary directory helper

struct TempDir {
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() / ("pyjuke_tests_" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

static void write_file(const fs::path& p, const Bytes& b)
{
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static void test_id3_files_and_resolve(const TempDir& td)
{
    section("id3 from files + resolve_metadata per-field fallback");
    fs::path music = td.path / "music";
    // v2 title only, v1 artist (and a title that must lose to v2), filename album.
    Bytes body;
    append(body, frame(3, "TIT2", text_latin1("V2 Title")));
    Bytes file = tag(3, 0, body, 64);
    Bytes audio(5000, 0x55);
    append(file, audio);
    append(file, v1("V1 Title", "V1 Artist", ""));
    fs::path p = music / "Some Artist" / "Some Album" / "track.mp3";
    write_file(p, file);

    CHECK_EQ(read_id3v2_file(p).title, std::string("V2 Title"));
    CHECK_EQ(read_id3v1_file(p).artist, std::string("V1 Artist"));
    TrackInfo info = resolve_metadata(p, music);
    CHECK_EQ(info.title, std::string("V2 Title"));
    CHECK_EQ(info.artist, std::string("V1 Artist"));
    CHECK_EQ(info.album, std::string("Some Album"));

    // Truncated file: header claims 1 MB, file ends early.
    Bytes trunc = tag(3, 0, body);
    Bytes s = syncsafe(1u << 20);
    std::copy(s.begin(), s.end(), trunc.begin() + 6);
    fs::path pt = music / "trunc.mp3";
    write_file(pt, trunc);
    CHECK_EQ(read_id3v2_file(pt).title, std::string("V2 Title"));
    assert(read_id3v1_file(pt).empty());

    // No tags at all: filename rule; tiny/empty/missing files.
    fs::path pn = music / "Art - Alb - Tit.mp3";
    write_file(pn, Bytes(10, 0));
    info = resolve_metadata(pn, music);
    CHECK_EQ(info.title, std::string("Tit"));
    CHECK_EQ(info.artist, std::string("Art"));
    CHECK_EQ(info.album, std::string("Alb"));
    write_file(music / "empty.mp3", Bytes{});
    assert(read_id3v2_file(music / "empty.mp3").empty());
    assert(read_id3v1_file(music / "missing.mp3").empty());
}

static void test_filename_metadata()
{
    section("filename -> metadata (1/2/3/4/5-part, relative to musicdir)");
    fs::path m = "/home/u/music";
    auto md = [&](const char* rel) { return metadata_from_filename(m / rel, m); };

    TrackInfo a = md("Artist/Album/Song_Name.mp3");
    CHECK_EQ(a.title, std::string("Song Name"));
    CHECK_EQ(a.artist, std::string("Artist"));
    CHECK_EQ(a.album, std::string("Album"));

    TrackInfo a2 = md("x/Artist/Album/01 Song.MP3");
    CHECK_EQ(a2.artist, std::string("Artist"));
    CHECK_EQ(a2.album, std::string("Album"));
    CHECK_EQ(a2.title, std::string("01 Song"));

    TrackInfo b = md("Album/Song.mp3");
    CHECK_EQ(b.title, std::string("Song"));
    CHECK_EQ(b.artist, std::string(""));
    CHECK_EQ(b.album, std::string("Album"));

    TrackInfo c = md("Song.mp3");
    CHECK_EQ(c.title, std::string("Song"));
    CHECK_EQ(c.artist, std::string(""));
    CHECK_EQ(c.album, std::string(""));

    TrackInfo d = md("dir/sub/The_Artist_-_The_Title.mp3");
    CHECK_EQ(d.artist, std::string("The Artist"));
    CHECK_EQ(d.title, std::string("The Title"));
    CHECK_EQ(d.album, std::string(""));

    TrackInfo e = md("dir/Art - Alb - Tit.mp3");
    CHECK_EQ(e.artist, std::string("Art"));
    CHECK_EQ(e.album, std::string("Alb"));
    CHECK_EQ(e.title, std::string("Tit"));

    TrackInfo f = md("Art - Alb - 03 - Tit.mp3");
    CHECK_EQ(f.artist, std::string("Art"));
    CHECK_EQ(f.album, std::string("Alb"));
    CHECK_EQ(f.title, std::string("Tit"));

    TrackInfo g = md("a - b - c - d - e.mp3");
    CHECK_EQ(g.title, std::string("e"));
    CHECK_EQ(g.artist, std::string(""));
    CHECK_EQ(g.album, std::string(""));

    // musicdir with trailing slash / dot components.
    TrackInfo h = metadata_from_filename("/home/u/music/A/B/t.mp3", "/home/u/./music/");
    CHECK_EQ(h.artist, std::string("A"));
    CHECK_EQ(h.album, std::string("B"));
    // Dots in name: only the extension is stripped.
    CHECK_EQ(md("v1.0.mp3").title, std::string("v1.0"));
}

static void test_scan(const TempDir& td)
{
    section("scan: recursion, case-insensitive .mp3, sorting, unreadable dir, symlink loop");
    fs::path root = td.path / "scan";
    write_file(root / "b" / "2.mp3", {});
    write_file(root / "b" / "1.MP3", {});
    write_file(root / "a" / "x" / "deep.Mp3", {});
    write_file(root / "a" / "notes.txt", {});
    write_file(root / "a" / "song.mp3.bak", {});
    write_file(root / "top.mp3", {});
    write_file(root / "locked" / "hidden.mp3", {});
    fs::create_directory_symlink(root, root / "a" / "loop");
    bool locked = ::chmod((root / "locked").c_str(), 0) == 0 && ::geteuid() != 0;

    ScanProgress prog;
    auto tracks = scan_library(root, &prog);
    ::chmod((root / "locked").c_str(), 0755);

    std::vector<fs::path> want{root / "a" / "x" / "deep.Mp3", root / "b" / "1.MP3", root / "b" / "2.mp3",
                               root / "top.mp3"};
    if (!locked) want.push_back(root / "locked" / "hidden.mp3");
    std::sort(want.begin(), want.end(), [](auto& x, auto& y) { return x.native() < y.native(); });
    CHECK_EQ(tracks.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) CHECK_EQ(tracks[i].string(), want[i].string());
    assert(prog.done.load());
    CHECK_EQ(prog.tracks_found.load(), want.size());
    assert(prog.entries_seen.load() >= 9);

    assert(scan_library(td.path / "does-not-exist").empty());
    assert(has_mp3_extension("A.mP3"));
    assert(!has_mp3_extension("mp3"));
    assert(!has_mp3_extension("x.mp4"));
}

// ---------------------------------------------------------------------------
// Player tests with a fake audio backend

struct FakeAudio : AudioControl {
    std::vector<std::string> loads;
    std::vector<bool> load_paused;
    std::vector<int64_t> seeks;
    int plays = 0, pauses = 0;
    int64_t pos = -1, dur = -1;
    bool ended = false, error = false;
    std::set<std::string> failing;  // paths whose load reports an error
    bool fail_all = false;

    void load(const std::string& path, bool start_paused) override
    {
        loads.push_back(path);
        load_paused.push_back(start_paused);
        ended = false;
        error = fail_all || failing.count(path) > 0;
        pos = error ? -1 : 0;
        dur = error ? -1 : 180000;
    }
    void play() override { ++plays; }
    void pause() override { ++pauses; }
    void seek_ms(int64_t ms) override
    {
        seeks.push_back(ms);
        pos = std::clamp<int64_t>(ms, 0, dur);
    }
    int64_t position_ms() const override { return pos; }
    int64_t duration_ms() const override { return dur; }
    bool take_track_ended() override { return std::exchange(ended, false); }
    bool take_track_error() override { return std::exchange(error, false); }
};

static std::vector<fs::path> make_tracks(size_t n)
{
    std::vector<fs::path> v;
    for (size_t i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "/m/t%05zu.mp3", i);
        v.emplace_back(buf);
    }
    return v;
}

static size_t index_of(const std::string& path) { return size_t(std::stoul(path.substr(4, 5))); }

static Player make_player(FakeAudio& fa, size_t n, uint64_t seed = 42)
{
    return Player(fa, make_tracks(n), "/m", PlayerTiming{}, seed,
                  [](const fs::path& p) { return TrackInfo{p.stem().string(), "artist", "album"}; });
}

static void test_format_time()
{
    section("format_time");
    CHECK_EQ(format_time(-1), std::string("--:--"));
    CHECK_EQ(format_time(0), std::string("00:00"));
    CHECK_EQ(format_time(999), std::string("00:00"));
    CHECK_EQ(format_time(9000), std::string("00:09"));
    CHECK_EQ(format_time(59999), std::string("00:59"));
    CHECK_EQ(format_time(60000), std::string("01:00"));
    CHECK_EQ(format_time(599000), std::string("09:59"));
    CHECK_EQ(format_time(600000), std::string("10:00"));
    CHECK_EQ(format_time(3599000), std::string("59:59"));
    CHECK_EQ(format_time(3600000), std::string("1:00:00"));
    CHECK_EQ(format_time(3725000), std::string("1:02:05"));
    CHECK_EQ(format_time(36000000), std::string("10:00:00"));
}

static void test_shuffle()
{
    section("shuffle: each index exactly once per pass, no repeat across passes");
    for (uint64_t seed : {1ull, 2ull, 99ull}) {
        for (size_t n : {1ul, 2ul, 7ul, 50ul}) {
            FakeAudio fa;
            Player p = make_player(fa, n, seed);
            p.start(0);
            for (size_t i = 1; i < n * 4; ++i) p.next(0);
            CHECK_EQ(fa.loads.size(), n * 4);
            for (size_t pass = 0; pass < 4; ++pass) {
                std::set<size_t> seen;
                for (size_t i = 0; i < n; ++i) seen.insert(index_of(fa.loads[pass * n + i]));
                CHECK_EQ(seen.size(), n);
            }
            if (n > 1)
                for (size_t i = 1; i < fa.loads.size(); ++i) assert(fa.loads[i] != fa.loads[i - 1]);
        }
    }
    // Different seeds give different orders (not a hard guarantee, but true here).
    FakeAudio f1, f2;
    Player p1 = make_player(f1, 50, 1), p2 = make_player(f2, 50, 2);
    p1.start(0);
    p2.start(0);
    for (int i = 0; i < 10; ++i) { p1.next(0); p2.next(0); }
    assert(f1.loads != f2.loads);
}

static void test_history()
{
    section("history: prev/next through shuffle history, truncation on new branch");
    FakeAudio fa;
    Player p = make_player(fa, 100);
    p.start(0);
    p.next(0);
    p.next(0);
    size_t A = index_of(fa.loads[0]), B = index_of(fa.loads[1]), C = index_of(fa.loads[2]);
    CHECK_EQ(p.history().size(), 3u);
    CHECK_EQ(p.current_index(), C);

    p.prev(0);
    CHECK_EQ(p.current_index(), B);
    p.prev(0);
    CHECK_EQ(p.current_index(), A);
    CHECK_EQ(p.history_cursor(), 0u);
    size_t loads_before = fa.loads.size();
    p.prev(0);  // nothing earlier: restart A
    CHECK_EQ(p.current_index(), A);
    CHECK_EQ(fa.loads.size(), loads_before + 1);
    CHECK_EQ(index_of(fa.loads.back()), A);

    p.next(0);
    CHECK_EQ(p.current_index(), B);
    p.next(0);
    CHECK_EQ(p.current_index(), C);
    CHECK_EQ(p.history().size(), 3u);  // replayed from history, nothing new drawn
    p.next(0);
    CHECK_EQ(p.history().size(), 4u);
    size_t D = p.current_index();
    assert(D != A && D != B && D != C);  // still the same shuffle pass

    // Back to B, then a non-shuffle move replaces the forward history.
    p.prev(0);
    p.prev(0);
    CHECK_EQ(p.current_index(), B);
    for (int i = 0; i < 4; ++i) p.change_mode();
    assert(p.mode() == Mode::Step);
    p.next(0);
    CHECK_EQ(p.current_index(), (B + 1) % 100);
    CHECK_EQ(p.history().size(), 3u);
    CHECK_EQ(p.history()[2], (B + 1) % 100);
    // Step mode prev is -1, not history.
    p.prev(0);
    CHECK_EQ(p.current_index(), B);
}

static void test_modes()
{
    section("modes: names, cycle, warp/fly/jump clamp, step wraps");
    FakeAudio fa;
    const size_t n = 2500;
    Player p = make_player(fa, n);
    CHECK_EQ(std::string(p.mode_name()), std::string("Random"));
    p.start(0);
    const char* names[] = {"Skip by 1000", "Skip by 100", "Skip by 10", "Step", "Random"};
    for (const char* nm : names) {
        p.change_mode();
        CHECK_EQ(std::string(p.mode_name()), std::string(nm));
    }

    struct Case { Mode m; size_t step; int changes; };
    for (Case c : {Case{Mode::Warp, 1000, 1}, Case{Mode::Fly, 100, 2}, Case{Mode::Jump, 10, 3}}) {
        FakeAudio f;
        Player q = make_player(f, n);
        q.start(0);
        for (int i = 0; i < c.changes; ++i) q.change_mode();
        assert(q.mode() == c.m);
        for (int guard = 0; guard < 1000 && q.current_index() != n - 1; ++guard) {
            size_t before = q.current_index();
            q.next(0);
            CHECK_EQ(q.current_index(), std::min(before + c.step, n - 1));
        }
        CHECK_EQ(q.current_index(), n - 1);
        q.next(0);
        CHECK_EQ(q.current_index(), n - 1);  // clamped
        for (int guard = 0; guard < 1000 && q.current_index() != 0; ++guard) {
            size_t before = q.current_index();
            q.prev(0);
            CHECK_EQ(q.current_index(), before >= c.step ? before - c.step : 0);
        }
        CHECK_EQ(q.current_index(), 0u);
        q.prev(0);
        CHECK_EQ(q.current_index(), 0u);
    }

    FakeAudio f;
    Player q = make_player(f, 5);
    q.start(0);
    for (int i = 0; i < 4; ++i) q.change_mode();
    assert(q.mode() == Mode::Step);
    while (q.current_index() != 4) q.next(0);
    q.next(0);
    CHECK_EQ(q.current_index(), 0u);  // wraps forward
    q.prev(0);
    CHECK_EQ(q.current_index(), 4u);  // wraps backward
}

static void test_prev_restart()
{
    section("prev: restart when position > reverse_skip_millis");
    FakeAudio fa;
    Player p = make_player(fa, 10);
    p.start(0);
    p.next(0);
    size_t cur = p.current_index();
    size_t loads = fa.loads.size();
    fa.pos = 4001;
    p.prev(0);
    CHECK_EQ(p.current_index(), cur);
    CHECK_EQ(fa.loads.size(), loads);
    CHECK_EQ(fa.seeks.back(), 0);
    fa.pos = 4000;  // not strictly greater: go to previous track
    p.prev(0);
    assert(p.current_index() != cur);
    CHECK_EQ(fa.loads.size(), loads + 1);
}

static void test_seek_timing()
{
    section("hold-to-seek: 500 ms wait, then +2000 every 100 ms; short press = next");
    {
        FakeAudio fa;
        Player p = make_player(fa, 10);
        p.start(0);
        size_t loads = fa.loads.size();
        fa.pos = 10000;
        p.seek_key_press(SeekDir::Forward, 1000);
        CHECK_EQ(p.next_deadline().value(), 1500);
        assert(!p.tick(1499));
        assert(fa.seeks.empty());
        assert(p.tick(1500));
        CHECK_EQ(fa.seeks.size(), 1u);
        CHECK_EQ(fa.seeks[0], 12000);
        assert(p.seeking());
        CHECK_EQ(p.next_deadline().value(), 1600);
        p.tick(1550);
        CHECK_EQ(fa.seeks.size(), 1u);
        p.tick(1600);
        CHECK_EQ(fa.seeks.size(), 2u);
        CHECK_EQ(fa.seeks[1], 14000);
        p.seek_key_release(SeekDir::Forward, 1650);
        assert(!p.next_deadline().has_value());
        CHECK_EQ(fa.loads.size(), loads);  // no track change after a hold
        p.tick(5000);
        CHECK_EQ(fa.seeks.size(), 2u);
    }
    {
        FakeAudio fa;
        Player p = make_player(fa, 10);
        p.start(0);
        size_t loads = fa.loads.size();
        p.seek_key_press(SeekDir::Forward, 0);
        p.tick(300);
        p.seek_key_release(SeekDir::Forward, 499);
        assert(fa.seeks.empty());
        CHECK_EQ(fa.loads.size(), loads + 1);  // short press = next
        p.tick(1000);
        assert(fa.seeks.empty());
    }
    section("hold-to-seek backward: -(delta + repeat), clamped at 0; short press = prev");
    {
        FakeAudio fa;
        Player p = make_player(fa, 10);
        p.start(0);
        p.next(0);
        size_t cur = p.current_index();
        fa.pos = 10000;
        p.seek_key_press(SeekDir::Backward, 0);
        p.tick(500);
        CHECK_EQ(fa.seeks.back(), 7900);
        fa.pos = 1000;
        p.tick(600);
        CHECK_EQ(fa.seeks.back(), 0);
        p.seek_key_release(SeekDir::Backward, 650);
        CHECK_EQ(p.current_index(), cur);

        fa.pos = 0;
        p.seek_key_press(SeekDir::Backward, 1000);
        p.seek_key_release(SeekDir::Backward, 1100);
        assert(p.current_index() != cur);  // prev (back in history)
    }
    section("on-screen seek buttons: immediate seek, repeat, release never skips");
    {
        FakeAudio fa;
        Player p = make_player(fa, 10);
        p.start(0);
        size_t loads = fa.loads.size();
        fa.pos = 30000;
        p.seek_button_press(SeekDir::Forward, 0);
        CHECK_EQ(fa.seeks.size(), 1u);
        CHECK_EQ(fa.seeks[0], 32000);
        CHECK_EQ(p.next_deadline().value(), 100);
        p.tick(100);
        CHECK_EQ(fa.seeks[1], 34000);
        p.seek_button_release(SeekDir::Forward, 120);
        CHECK_EQ(fa.loads.size(), loads);
        p.seek_button_press(SeekDir::Backward, 200);
        p.seek_button_release(SeekDir::Backward, 210);
        CHECK_EQ(fa.seeks.back(), 34000 - 2100);
        CHECK_EQ(fa.loads.size(), loads);
    }
    section("scrollbar seek in thousandths");
    {
        FakeAudio fa;
        Player p = make_player(fa, 3);
        p.start(0);
        p.scroll_seek(500);
        CHECK_EQ(fa.seeks.back(), 90000);
        p.scroll_seek(1500);
        CHECK_EQ(fa.seeks.back(), 180000);
        p.scroll_seek(-3);
        CHECK_EQ(fa.seeks.back(), 0);
        fa.pos = 45000;
        CHECK_EQ(p.scroll_value(), 250);
    }
}

static void test_state_and_end()
{
    section("toggle play/pause, paused loads, end-of-track advance");
    FakeAudio fa;
    Player p = make_player(fa, 10);
    assert(p.state() == PlayState::Stopped);
    CHECK_EQ(std::string(p.state_name()), std::string("Stopped"));
    p.start(0);
    assert(p.state() == PlayState::Playing);
    CHECK_EQ(std::string(p.state_name()), std::string(""));
    assert(!fa.load_paused.back());
    p.toggle_play();
    assert(p.state() == PlayState::Paused);
    CHECK_EQ(fa.pauses, 1);
    p.next(0);
    assert(fa.load_paused.back());  // stays paused on track change
    p.toggle_play();
    CHECK_EQ(fa.plays, 1);
    assert(p.state() == PlayState::Playing);

    size_t loads = fa.loads.size();
    uint64_t serial = p.track_serial();
    fa.ended = true;
    assert(p.tick(0));
    CHECK_EQ(fa.loads.size(), loads + 1);
    assert(p.track_serial() > serial);
    CHECK_EQ(p.info().artist, std::string("artist"));

    // Empty library: nothing crashes, nothing loads.
    FakeAudio fe;
    Player pe = make_player(fe, 0);
    pe.start(0);
    pe.next(0);
    pe.prev(0);
    pe.toggle_play();
    pe.seek_key_press(SeekDir::Forward, 0);
    pe.tick(1000);
    pe.seek_key_release(SeekDir::Forward, 1001);
    pe.scroll_seek(10);
    assert(fe.loads.empty());
    assert(!pe.has_track());
    CHECK_EQ(pe.current_path().string(), std::string(""));
}

static void test_error_skip()
{
    section("errors: skip to next, stop when everything fails, no infinite loop");
    {
        FakeAudio fa;
        fa.fail_all = true;
        Player p = make_player(fa, 6);
        p.start(0);
        for (int i = 0; i < 100 && p.state() != PlayState::Stopped; ++i) p.tick(i);
        assert(p.state() == PlayState::Stopped);
        assert(p.stopped_on_errors());
        CHECK_EQ(fa.loads.size(), 6u);
        size_t loads = fa.loads.size();
        for (int i = 0; i < 10; ++i) p.tick(1000 + i);
        CHECK_EQ(fa.loads.size(), loads);  // stays stopped
        p.toggle_play();                   // user retry
        assert(p.state() == PlayState::Playing);
        CHECK_EQ(fa.loads.size(), loads + 1);
    }
    {
        // Some failures in step mode: skip past them to a good track.
        FakeAudio fa;
        auto tr = make_tracks(10);
        Player p = make_player(fa, 10);
        for (int i = 0; i < 4; ++i) p.change_mode();
        assert(p.mode() == Mode::Step);
        fa.failing = {tr[1].string(), tr[2].string(), tr[3].string()};
        p.start(0);  // step mode from index 0 -> starts at 1
        for (int i = 0; i < 20; ++i) p.tick(i);
        CHECK_EQ(p.current_index(), 4u);
        assert(p.state() == PlayState::Playing);
    }
    {
        // Clamping mode at the last track: don't retry the same file.
        FakeAudio fa;
        auto tr = make_tracks(30);
        Player p = make_player(fa, 30);
        for (int i = 0; i < 2; ++i) p.change_mode();
        assert(p.mode() == Mode::Fly);
        p.start(0);
        while (p.current_index() != 29) p.next(0);
        fa.failing = {tr[29].string()};
        size_t loads = fa.loads.size();
        p.toggle_play();  // pause
        p.toggle_play();  // resume (no reload)
        p.prev(0);
        p.next(0);  // reload 29, which now fails
        CHECK_EQ(fa.loads.size(), loads + 2);
        for (int i = 0; i < 20; ++i) p.tick(i);
        assert(p.state() == PlayState::Stopped);
        CHECK_EQ(fa.loads.size(), loads + 2);
    }
    {
        // Shuffle with a single bad track among good ones.
        FakeAudio fa;
        auto tr = make_tracks(3);
        fa.failing = {tr[0].string()};
        Player p = make_player(fa, 3, 7);
        p.start(0);
        for (int i = 0; i < 10; ++i) p.tick(i);
        assert(p.state() == PlayState::Playing);
        assert(p.current_index() != 0);
    }
}

// ---------------------------------------------------------------------------

static void test_reload_current()
{
    section("reload_current: same track and position on a new output, state kept");
    FakeAudio a, b;
    AudioSwitch sw(a);
    auto meta = [](const fs::path& f) { return TrackInfo{f.stem().string(), "", ""}; };
    Player p(sw, make_tracks(3), "/m", PlayerTiming{}, 1, meta);
    p.reload_current(1000);  // nothing playing yet: no-op
    assert(a.loads.empty());
    p.start(0);
    CHECK_EQ(a.loads.size(), size_t(1));
    std::string current = p.current_path().string();
    uint64_t serial = p.track_serial();
    a.pos = 42000;

    int64_t pos = p.position_ms();  // read before switching, as the app does
    sw.set_target(b);
    p.reload_current(pos);
    CHECK_EQ(b.loads.size(), size_t(1));
    CHECK_EQ(b.loads[0], current);
    assert(!b.load_paused[0]);
    CHECK_EQ(b.seeks.size(), size_t(1));
    CHECK_EQ(b.seeks[0], int64_t(42000));
    CHECK_EQ(a.loads.size(), size_t(1));  // the old output was not touched
    assert(p.state() == PlayState::Playing);
    assert(p.track_serial() > serial);
    CHECK_EQ(p.current_path().string(), current);

    // Paused: the new output loads paused, position 0 means no seek.
    p.toggle_play();
    assert(p.state() == PlayState::Paused);
    sw.set_target(a);
    p.reload_current(0);
    CHECK_EQ(a.loads.size(), size_t(2));
    assert(a.load_paused[1]);
    CHECK_EQ(a.seeks.size(), size_t(0));
    assert(p.state() == PlayState::Paused);

    // Stopped (every track failed): nothing happens.
    FakeAudio c;
    c.fail_all = true;
    AudioSwitch sw2(c);
    Player q(sw2, make_tracks(2), "/m", PlayerTiming{}, 1, meta);
    q.start(0);
    while (q.state() != PlayState::Stopped) q.tick(0);
    size_t loads = c.loads.size();
    q.reload_current(5000);
    CHECK_EQ(c.loads.size(), loads);
}

static void test_config()
{
    section("config parsing");
    Config d = default_config();
    const char* home = std::getenv("HOME");
    if (home) CHECK_EQ(d.musicdir, std::string(home) + "/music");
    CHECK_EQ(d.width, 800);
    CHECK_EQ(d.height, 480);
    CHECK_EQ(d.font_size, 36);
    assert(d.font.empty());
    assert(!d.fullscreen);
    CHECK_EQ(d.seek_wait_millis, 500);
    CHECK_EQ(d.seek_repeat_millis, 100);
    CHECK_EQ(d.seek_delta_millis, 2000);
    CHECK_EQ(d.reverse_skip_millis, 4000);
    CHECK_EQ(d.status_repeat_millis, 200);
    CHECK_EQ(d.clock_repeat_millis, 900);
    assert(d.outputs);
    CHECK_EQ(d.stream_port, 0);
    assert(d.default_output.empty());
    CHECK_EQ(d.local_volume, std::string("system"));

    std::istringstream in(
        "# comment\n"
        "\n"
        "   musicdir = /data/My Music  \n"
        "fullscreen=yes\n"
        "width = 1024\n"
        "Height = 600\n"
        "font = \"~/fonts/Some Font.ttf\"\n"
        "font_size = 40\n"
        "seek_wait_millis = 250\n"
        "seek_repeat_millis = 50\n"
        "seek_delta_millis = 5000\n"
        "reverse_skip_millis = 3000\n"
        "status_repeat_millis = 250\n"
        "clock_repeat_millis = 1000\n"
        "outputs = off\n"
        "stream_port = 49200\n"
        "default_output = Living Room\n"
        "local_volume = App\n"
        "bogus = 1\n"
        "width = wide\n"
        "no equals sign\n"
        "fullscreen = maybe\n");
    std::ostringstream warn;
    Config c = parse_config(in, d, warn, "test.cfg");
    CHECK_EQ(c.musicdir, std::string("/data/My Music"));
    assert(c.fullscreen);
    CHECK_EQ(c.width, 1024);
    CHECK_EQ(c.height, 600);
    if (home) CHECK_EQ(c.font, std::string(home) + "/fonts/Some Font.ttf");
    CHECK_EQ(c.font_size, 40);
    CHECK_EQ(c.seek_wait_millis, 250);
    CHECK_EQ(c.seek_repeat_millis, 50);
    CHECK_EQ(c.seek_delta_millis, 5000);
    CHECK_EQ(c.reverse_skip_millis, 3000);
    CHECK_EQ(c.status_repeat_millis, 250);
    CHECK_EQ(c.clock_repeat_millis, 1000);
    assert(!c.outputs);
    CHECK_EQ(c.stream_port, 49200);
    CHECK_EQ(c.default_output, std::string("Living Room"));
    CHECK_EQ(c.local_volume, std::string("app"));
    std::string w = warn.str();
    assert(w.find("test.cfg:19: unknown key 'bogus'") != std::string::npos);
    assert(w.find("test.cfg:20: invalid value 'wide'") != std::string::npos);
    assert(w.find("test.cfg:21:") != std::string::npos);
    assert(w.find("test.cfg:22: invalid value 'maybe'") != std::string::npos);
    std::istringstream bad_lv("local_volume = loud\n");
    std::ostringstream warn2;
    Config lv = parse_config(bad_lv, d, warn2, "t");
    CHECK_EQ(lv.local_volume, std::string("system"));
    assert(warn2.str().find("invalid value 'loud'") != std::string::npos);

    // Missing file -> defaults, no throw.
    Config m = load_config("/nonexistent/pyjuke/config");
    CHECK_EQ(m.width, 800);
}

int main()
{
    std::printf("pyjuke_tests\n");
    TempDir td;
    test_id3();
    test_id3_files_and_resolve(td);
    test_filename_metadata();
    test_scan(td);
    test_format_time();
    test_shuffle();
    test_history();
    test_modes();
    test_prev_restart();
    test_seek_timing();
    test_state_and_end();
    test_error_skip();
    test_reload_current();
    test_config();
    run_network_tests(td.path);
    std::printf("all tests passed\n");
    return 0;
}
