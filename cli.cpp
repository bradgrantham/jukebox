// cli.cpp - pyjuke-cli: headless checks of the decoder, audio thread and scanner.
//
//   pyjuke-cli --decode FILE OUT.wav   decode whole file to 16-bit PCM WAV
//   pyjuke-cli --play FILE             play; stdin commands:
//                                        p      pause / resume
//                                        s N    seek to N seconds
//                                        q      quit
//                                      prints position once a second and
//                                      exits when the track ends
//   pyjuke-cli --scan DIR [-v]         count tracks; -v lists each with
//                                      its resolved metadata

#include "audio.h"
#include "library.h"
#include "player.h"

#include "minimp3_ex.h"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace pyjuke;

namespace {

int usage()
{
    std::fprintf(stderr,
                 "usage: pyjuke-cli --decode FILE OUT.wav\n"
                 "       pyjuke-cli --play FILE\n"
                 "       pyjuke-cli --scan DIR [-v]\n");
    return 2;
}

void put_le(std::ofstream& o, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) o.put(static_cast<char>((v >> (8 * i)) & 0xFF));
}

int cmd_decode(const char* in_path, const char* out_path)
{
    mp3dec_ex_t dec;
    std::memset(&dec, 0, sizeof dec);
    if (int r = mp3dec_ex_open(&dec, in_path, MP3D_SEEK_TO_SAMPLE); r != 0) {
        std::fprintf(stderr, "%s: open failed (minimp3 error %d)\n", in_path, r);
        return 1;
    }
    if (dec.info.hz <= 0 || dec.info.channels <= 0) {
        std::fprintf(stderr, "%s: no MPEG audio frames\n", in_path);
        mp3dec_ex_close(&dec);
        return 1;
    }
    std::vector<mp3d_sample_t> pcm;
    std::vector<mp3d_sample_t> chunk(4096 * 2);
    for (;;) {
        size_t got = mp3dec_ex_read(&dec, chunk.data(), chunk.size());
        pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<long>(got));
        if (got < chunk.size()) break;
    }
    int err = dec.last_error;
    const int ch = dec.info.channels, hz = dec.info.hz;
    const uint64_t expected = dec.samples;
    mp3dec_ex_close(&dec);

    std::ofstream o(out_path, std::ios::binary);
    if (!o) {
        std::fprintf(stderr, "%s: cannot write\n", out_path);
        return 1;
    }
    uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * 2);
    o.write("RIFF", 4);
    put_le(o, 36 + data_bytes, 4);
    o.write("WAVEfmt ", 8);
    put_le(o, 16, 4);
    put_le(o, 1, 2);  // PCM
    put_le(o, static_cast<uint32_t>(ch), 2);
    put_le(o, static_cast<uint32_t>(hz), 4);
    put_le(o, static_cast<uint32_t>(hz * ch * 2), 4);
    put_le(o, static_cast<uint32_t>(ch * 2), 2);
    put_le(o, 16, 2);
    o.write("data", 4);
    put_le(o, data_bytes, 4);
    for (mp3d_sample_t s : pcm) put_le(o, static_cast<uint16_t>(s), 2);

    std::printf("rate %d channels %d frames %zu (decoder predicted %llu)%s\n", hz, ch, pcm.size() / ch,
                static_cast<unsigned long long>(expected / static_cast<uint64_t>(ch)),
                err ? " [decode error]" : "");
    return err ? 1 : 0;
}

const char* state_str(AudioState s)
{
    switch (s) {
    case AudioState::Stopped: return "stopped";
    case AudioState::Playing: return "playing";
    case AudioState::Paused: return "paused";
    }
    return "?";
}

int cmd_play(const char* path)
{
    std::atomic<bool> woke{false};
    AudioEngine audio([&] { woke.store(true); });
    audio.load(path, false);

    using clock = std::chrono::steady_clock;
    auto last_print = clock::now() - std::chrono::seconds(1);
    bool stdin_open = true;
    std::string pending;
    bool paused = false;

    for (;;) {
        struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
        int r = stdin_open ? ::poll(&pfd, 1, 50) : (std::this_thread::sleep_for(std::chrono::milliseconds(50)), 0);
        if (r > 0) {
            char buf[256];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) {
                stdin_open = false;
            } else {
                pending.append(buf, static_cast<size_t>(n));
            }
        }
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (line.empty()) continue;
            if (line[0] == 'q') {
                std::printf("quit\n");
                return 0;
            } else if (line[0] == 'p') {
                paused = !paused;
                if (paused) audio.pause();
                else audio.play();
                std::printf("%s\n", paused ? "pause" : "resume");
            } else if (line[0] == 's') {
                double sec = std::atof(line.c_str() + 1);
                audio.seek_ms(static_cast<int64_t>(sec * 1000.0));
                std::printf("seek %.3f s\n", sec);
            } else {
                std::printf("? (p, s N, q)\n");
            }
            std::fflush(stdout);
        }

        if (woke.exchange(false)) {
            if (audio.take_track_error()) {
                std::printf("error: %s\n", audio.last_error().c_str());
                return 1;
            }
            if (audio.take_track_ended()) {
                std::printf("end of track at %s (%lld ms)\n", format_time(audio.position_ms()).c_str(),
                            static_cast<long long>(audio.position_ms()));
                return 0;
            }
        }

        auto now = clock::now();
        if (now - last_print >= std::chrono::seconds(1)) {
            last_print = now;
            std::printf("pos %s / %s (%lld ms) %s %d Hz %d ch\n", format_time(audio.position_ms()).c_str(),
                        format_time(audio.duration_ms()).c_str(), static_cast<long long>(audio.position_ms()),
                        state_str(audio.state()), audio.sample_rate(), audio.channels());
            std::fflush(stdout);
        }
    }
}

int cmd_scan(const char* dir, bool verbose)
{
    ScanProgress progress;
    std::vector<std::filesystem::path> tracks;
    std::thread worker([&] { tracks = scan_library(dir, &progress); });
    while (!progress.done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (verbose)
            std::fprintf(stderr, "\rread %zu entries, %zu tracks", progress.entries_seen.load(),
                         progress.tracks_found.load());
    }
    worker.join();
    if (verbose) std::fprintf(stderr, "\n");
    std::printf("%zu tracks\n", tracks.size());
    if (verbose) {
        for (size_t i = 0; i < tracks.size(); ++i) {
            TrackInfo info = resolve_metadata(tracks[i], dir);
            std::printf("%5zu  %s\n       title [%s]  artist [%s]  album [%s]\n", i, tracks[i].c_str(),
                        info.title.c_str(), info.artist.c_str(), info.album.c_str());
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    if (cmd == "--decode" && argc == 4) return cmd_decode(argv[2], argv[3]);
    if (cmd == "--play" && argc == 3) return cmd_play(argv[2]);
    if (cmd == "--scan" && (argc == 3 || argc == 4)) {
        bool verbose = false;
        const char* dir = nullptr;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "-v") == 0) verbose = true;
            else dir = argv[i];
        }
        if (!dir) return usage();
        return cmd_scan(dir, verbose);
    }
    return usage();
}
