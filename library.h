// library.h - music directory scan and per-track metadata.
//
// scan_library() is meant to run on a worker thread while the UI draws a
// "Populating Playlist... have read N files" screen from a ScanProgress the
// two threads share. Metadata is resolved lazily, per track, when the track
// becomes current (resolve_metadata()).

#ifndef PYJUKE_LIBRARY_H
#define PYJUKE_LIBRARY_H

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace pyjuke {

// Shared between the scanning thread (writer) and the UI (reader).
struct ScanProgress {
    std::atomic<size_t> entries_seen{0};  // every directory entry examined
    std::atomic<size_t> tracks_found{0};  // .mp3 files accepted so far
    std::atomic<bool> cancel{false};      // set by the UI to abandon the scan
    std::atomic<bool> done{false};        // set by scan_library on return
};

// Recursively collect regular files under `root` whose extension is ".mp3"
// (any case). Symlinks are followed; a directory reached twice (symlink loop)
// is visited once. Unreadable directories and entries are skipped silently.
// The result is sorted by full path (byte-wise), so the index of a track is
// stable and matches the on-screen track number. `progress` may be null.
// If progress->cancel becomes true, returns what was found so far, sorted.
std::vector<std::filesystem::path> scan_library(const std::filesystem::path& root,
                                                ScanProgress* progress = nullptr);

// Displayable metadata for one track (UTF-8; any field may be empty).
struct TrackInfo {
    std::string title;
    std::string artist;
    std::string album;
};

// The old pyjuke filename rule (port of UpdateTrackInfo). Components are
// taken relative to `musicdir` (if `track` is not under it, the whole path is
// used). The file name has its extension stripped, '_' -> ' ', and is split
// on " - ":
//   1 part : title; artist = grandparent dir, album = parent dir
//            (only album if there is just one dir; neither if none)
//   2 parts: artist - title
//   3 parts: artist - album - title
//   4 parts: artist - album - (track no.) - title
//   5+     : title = last part, artist/album empty
TrackInfo metadata_from_filename(const std::filesystem::path& track,
                                 const std::filesystem::path& musicdir);

// Per field: ID3v2 if present and non-empty, else ID3v1, else the filename
// rule. Reads at most the ID3v2 tag and the last 128 bytes of the file.
TrackInfo resolve_metadata(const std::filesystem::path& track,
                           const std::filesystem::path& musicdir);

// True if `p` has a ".mp3" extension, case-insensitive.
bool has_mp3_extension(const std::filesystem::path& p);

} // namespace pyjuke

#endif
