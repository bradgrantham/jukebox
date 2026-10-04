// library.cpp - see library.h.

#include "library.h"

#include "id3.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace pyjuke {

bool has_mp3_extension(const fs::path& p)
{
    std::string ext = p.extension().string();
    if (ext.size() != 4) return false;
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".mp3";
}

std::vector<fs::path> scan_library(const fs::path& root, ScanProgress* progress)
{
    std::vector<fs::path> out;
    std::set<fs::path> visited;  // canonical directory paths
    std::vector<fs::path> stack{root};

    while (!stack.empty()) {
        if (progress && progress->cancel.load()) break;
        fs::path dir = std::move(stack.back());
        stack.pop_back();

        std::error_code ec;
        fs::path canon = fs::canonical(dir, ec);
        if (ec) continue;
        if (!visited.insert(canon).second) continue;

        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;
        for (fs::directory_iterator end; it != end; it.increment(ec)) {
            if (ec) break;
            if (progress) {
                progress->entries_seen.fetch_add(1, std::memory_order_relaxed);
                if (progress->cancel.load(std::memory_order_relaxed)) break;
            }
            const fs::directory_entry& e = *it;
            std::error_code ec2;
            // status() follows symlinks, as os.path.isdir/isfile did.
            fs::file_status st = e.status(ec2);
            if (ec2) continue;
            if (fs::is_directory(st)) {
                stack.push_back(e.path());
            } else if (fs::is_regular_file(st) && has_mp3_extension(e.path())) {
                out.push_back(e.path());
                if (progress) progress->tracks_found.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::sort(out.begin(), out.end(),
              [](const fs::path& a, const fs::path& b) { return a.native() < b.native(); });
    if (progress) progress->done.store(true);
    return out;
}

namespace {

std::vector<std::string> split_on(const std::string& s, const std::string& sep)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t at = s.find(sep, start);
        if (at == std::string::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, at - start));
        start = at + sep.size();
    }
}

} // namespace

TrackInfo metadata_from_filename(const fs::path& track, const fs::path& musicdir)
{
    fs::path rel = track.lexically_normal().lexically_relative(musicdir.lexically_normal());
    if (rel.empty() || *rel.begin() == "..") rel = track.relative_path();

    std::vector<std::string> names;
    for (const auto& c : rel) {
        std::string s = c.string();
        if (!s.empty() && s != ".") names.push_back(s);
    }
    TrackInfo info;
    if (names.empty()) return info;

    std::string filepart = fs::path(names.back()).stem().string();
    std::replace(filepart.begin(), filepart.end(), '_', ' ');
    std::vector<std::string> parts = split_on(filepart, " - ");

    info.title = parts.back();
    const size_t nn = names.size();
    switch (parts.size()) {
    case 1:
        if (nn > 2) {
            info.artist = names[nn - 3];
            info.album = names[nn - 2];
        } else if (nn > 1) {
            info.album = names[nn - 2];
        }
        break;
    case 2:
        info.artist = parts[0];
        break;
    case 3:
    case 4:
        info.artist = parts[0];
        info.album = parts[1];
        break;
    default:
        break;
    }
    info.title = sanitize_utf8(info.title);
    info.artist = sanitize_utf8(info.artist);
    info.album = sanitize_utf8(info.album);
    return info;
}

TrackInfo resolve_metadata(const fs::path& track, const fs::path& musicdir)
{
    Id3Tags v2 = read_id3v2_file(track);
    TrackInfo info{v2.title, v2.artist, v2.album};
    if (info.title.empty() || info.artist.empty() || info.album.empty()) {
        Id3Tags v1 = read_id3v1_file(track);
        if (info.title.empty()) info.title = v1.title;
        if (info.artist.empty()) info.artist = v1.artist;
        if (info.album.empty()) info.album = v1.album;
    }
    if (info.title.empty() || info.artist.empty() || info.album.empty()) {
        TrackInfo fn = metadata_from_filename(track, musicdir);
        if (info.title.empty()) info.title = fn.title;
        if (info.artist.empty()) info.artist = fn.artist;
        if (info.album.empty()) info.album = fn.album;
    }
    return info;
}

} // namespace pyjuke
