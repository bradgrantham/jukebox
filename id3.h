// id3.h - minimal ID3 tag reader: title, artist and album only.
//
// Supports ID3v2.2 (TT2/TP1/TAL), ID3v2.3 and ID3v2.4 (TIT2/TPE1/TALB) with
// text encodings ISO-8859-1, UTF-16 with BOM, UTF-16BE and UTF-8; tag-level
// unsynchronisation (v2.2/v2.3/v2.4), per-frame unsynchronisation and data
// length indicators (v2.4), extended headers, syncsafe frame sizes (v2.4,
// with a fallback for writers that used plain big-endian sizes). Compressed
// or encrypted frames are skipped. Also ID3v1/v1.1 (last 128 bytes, Latin-1).
//
// All output is valid UTF-8 (invalid input sequences become U+FFFD), with
// trailing NULs and trailing whitespace removed. A field that is absent or
// cannot be decoded is left empty. Malformed or truncated input never reads
// out of bounds; parsing simply stops.

#ifndef PYJUKE_ID3_H
#define PYJUKE_ID3_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace pyjuke {

struct Id3Tags {
    std::string title;
    std::string artist;
    std::string album;

    bool empty() const { return title.empty() && artist.empty() && album.empty(); }
};

// Parse an ID3v2 tag from memory. `data` must start at the "ID3" header; it
// may be shorter than the size the header claims (the rest is ignored).
Id3Tags parse_id3v2(const uint8_t* data, size_t size);

// Parse a 128-byte ID3v1 block ("TAG" + fields). Returns empty tags if
// `size` < 128 or the magic is wrong.
Id3Tags parse_id3v1(const uint8_t* data, size_t size);

// Read the ID3v2 tag at the start of a file. Reads only the 10-byte header
// and the tag body (capped at kMaxId3v2Read bytes). Empty on any error.
Id3Tags read_id3v2_file(const std::filesystem::path& path);
// Read the ID3v1 tag in the last 128 bytes of a file. Empty on any error.
Id3Tags read_id3v1_file(const std::filesystem::path& path);

// Upper bound on how much of an ID3v2 tag body is read from disk. Tags with
// huge embedded pictures before the text frames lose those text frames.
constexpr size_t kMaxId3v2Read = 16u * 1024u * 1024u;

// Helpers exposed for tests and for library.cpp.
// Append code point `cp` to `out` as UTF-8 (invalid code points -> U+FFFD).
void append_utf8(std::string& out, uint32_t cp);
// Convert ISO-8859-1 bytes to UTF-8.
std::string latin1_to_utf8(const uint8_t* p, size_t n);
// Return `s` with invalid UTF-8 sequences replaced by U+FFFD.
std::string sanitize_utf8(const std::string& s);

} // namespace pyjuke

#endif
