// id3.cpp - see id3.h.
//
// References: ID3v2.2 (id3.org "id3v2-00"), ID3v2.3.0 and ID3v2.4.0
// structure/frames documents, ID3v1 layout.

#include "id3.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace pyjuke {

void append_utf8(std::string& out, uint32_t cp)
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string latin1_to_utf8(const uint8_t* p, size_t n)
{
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) append_utf8(out, p[i]);
    return out;
}

std::string sanitize_utf8(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    size_t n = s.size(), i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        int len = 0;
        uint32_t cp = 0, min = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
        bool ok = len > 0 && i + static_cast<size_t>(len) <= n;
        for (int k = 1; ok && k < len; ++k) {
            uint8_t cc = p[i + static_cast<size_t>(k)];
            if ((cc & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
        if (ok) {
            out.append(s, i, static_cast<size_t>(len));
            i += static_cast<size_t>(len);
        } else {
            append_utf8(out, 0xFFFD);
            ++i;
        }
    }
    return out;
}

namespace {

void trim_trailing(std::string& s)
{
    while (!s.empty()) {
        char c = s.back();
        if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n') s.pop_back();
        else break;
    }
}

std::string decode_utf16(const uint8_t* p, size_t n, bool big_endian)
{
    std::string out;
    size_t i = 0;
    auto unit = [&](size_t at) -> uint32_t {
        return big_endian ? (uint32_t(p[at]) << 8) | p[at + 1] : (uint32_t(p[at + 1]) << 8) | p[at];
    };
    while (i + 1 < n) {
        uint32_t u = unit(i);
        i += 2;
        if (u == 0) break;  // terminator / start of a second value
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 < n) {
                uint32_t lo = unit(i);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    i += 2;
                    append_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                    continue;
                }
            }
            append_utf8(out, 0xFFFD);
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            append_utf8(out, 0xFFFD);
        } else {
            append_utf8(out, u);
        }
    }
    return out;
}

// Decode a text frame body: encoding byte followed by the text. Only the first
// of several NUL-separated values (v2.4) is returned.
std::string decode_text_frame(const uint8_t* p, size_t n)
{
    if (n < 1) return {};
    uint8_t enc = p[0];
    ++p;
    --n;
    std::string out;
    switch (enc) {
    case 0: {
        size_t len = 0;
        while (len < n && p[len] != 0) ++len;
        out = latin1_to_utf8(p, len);
        break;
    }
    case 1:
    case 2: {
        bool be = (enc == 2);
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = false; p += 2; n -= 2; }
        else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = true; p += 2; n -= 2; }
        // Encoding 1 without a BOM is out of spec; little-endian is what the
        // offending (mostly Windows) writers produce.
        out = decode_utf16(p, n, be);
        break;
    }
    case 3: {
        size_t len = 0;
        while (len < n && p[len] != 0) ++len;
        out = sanitize_utf8(std::string(reinterpret_cast<const char*>(p), len));
        break;
    }
    default:
        return {};
    }
    trim_trailing(out);
    return out;
}

std::vector<uint8_t> remove_unsync(const uint8_t* p, size_t n)
{
    std::vector<uint8_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(p[i]);
        if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00) ++i;
    }
    return out;
}

uint32_t be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }
bool is_syncsafe(const uint8_t* p) { return ((p[0] | p[1] | p[2] | p[3]) & 0x80) == 0; }
uint32_t syncsafe32(const uint8_t* p)
{
    return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) | (uint32_t(p[2] & 0x7F) << 7) |
           (p[3] & 0x7F);
}

bool valid_id_char(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

enum class Field { None, Title, Artist, Album };

Field field_for(const uint8_t* id, int major)
{
    if (major == 2) {
        if (!std::memcmp(id, "TT2", 3)) return Field::Title;
        if (!std::memcmp(id, "TP1", 3)) return Field::Artist;
        if (!std::memcmp(id, "TAL", 3)) return Field::Album;
    } else {
        if (!std::memcmp(id, "TIT2", 4)) return Field::Title;
        if (!std::memcmp(id, "TPE1", 4)) return Field::Artist;
        if (!std::memcmp(id, "TALB", 4)) return Field::Album;
    }
    return Field::None;
}

} // namespace

Id3Tags parse_id3v2(const uint8_t* data, size_t size)
{
    Id3Tags tags;
    if (!data || size < 10 || std::memcmp(data, "ID3", 3) != 0) return tags;
    const int major = data[3];
    if (major < 2 || major > 4) return tags;
    const uint8_t flags = data[5];
    if (!is_syncsafe(data + 6)) return tags;
    const size_t tag_size = syncsafe32(data + 6);
    const size_t avail = std::min(tag_size, size - 10);

    const bool tag_unsync = (flags & 0x80) != 0;
    if (major == 2 && (flags & 0x40)) return tags;  // v2.2 compression: undefined scheme

    // v2.2/v2.3 apply unsynchronisation to the whole tag body; v2.4 applies it
    // per frame (the header flag means "every frame is unsynchronised").
    std::vector<uint8_t> b;
    if (tag_unsync && major < 4) b = remove_unsync(data + 10, avail);
    else b.assign(data + 10, data + 10 + avail);

    size_t pos = 0;
    if (flags & 0x40) {  // extended header (v2.3/v2.4)
        if (b.size() < 4) return tags;
        if (major == 3) {
            size_t ext = be32(b.data());  // excludes its own 4 size bytes
            if (ext > b.size() - 4) return tags;
            pos = 4 + ext;
        } else {
            if (!is_syncsafe(b.data())) return tags;
            size_t ext = syncsafe32(b.data());  // includes the size bytes
            if (ext < 6 || ext > b.size()) return tags;
            pos = ext;
        }
    }

    const size_t hdr = (major == 2) ? 6 : 10;
    const size_t id_len = (major == 2) ? 3 : 4;
    const size_t end = b.size();

    auto frame_start_ok = [&](size_t at) {
        if (at == end) return true;
        if (at > end) return false;
        if (b[at] == 0) return true;  // padding
        if (at + hdr > end) return false;
        for (size_t k = 0; k < id_len; ++k)
            if (!valid_id_char(b[at + k])) return false;
        return true;
    };

    while (pos + hdr <= end) {
        const uint8_t* fh = b.data() + pos;
        if (fh[0] == 0) break;  // padding
        bool id_ok = true;
        for (size_t k = 0; k < id_len; ++k) id_ok = id_ok && valid_id_char(fh[k]);
        if (!id_ok) break;

        size_t fsize;
        uint16_t fflags = 0;
        if (major == 2) {
            fsize = be24(fh + 3);
        } else if (major == 3) {
            fsize = be32(fh + 4);
            fflags = static_cast<uint16_t>((fh[8] << 8) | fh[9]);
        } else {
            fflags = static_cast<uint16_t>((fh[8] << 8) | fh[9]);
            size_t plain = be32(fh + 4);
            if (is_syncsafe(fh + 4)) {
                size_t ss = syncsafe32(fh + 4);
                // Some writers (old iTunes) used plain sizes in v2.4 tags.
                bool ss_ok = pos + hdr + ss <= end && frame_start_ok(pos + hdr + ss);
                bool plain_ok = pos + hdr + plain <= end && frame_start_ok(pos + hdr + plain);
                fsize = (!ss_ok && plain_ok) ? plain : ss;
            } else {
                fsize = plain;
            }
        }
        pos += hdr;
        if (fsize > end - pos) break;  // truncated
        const uint8_t* fd = b.data() + pos;
        size_t fn = fsize;
        pos += fsize;

        Field f = field_for(fh, major);
        if (f == Field::None) continue;

        std::vector<uint8_t> tmp;
        if (major == 3) {
            if (fflags & 0x00C0) continue;  // compressed or encrypted
            if (fflags & 0x0020) {          // grouping identity byte
                if (fn < 1) continue;
                ++fd;
                --fn;
            }
        } else if (major == 4) {
            if (fflags & 0x000C) continue;  // compressed or encrypted
            if (fflags & 0x0040) {          // grouping identity byte
                if (fn < 1) continue;
                ++fd;
                --fn;
            }
            if (fflags & 0x0001) {          // data length indicator
                if (fn < 4) continue;
                fd += 4;
                fn -= 4;
            }
            if ((fflags & 0x0002) || tag_unsync) {
                tmp = remove_unsync(fd, fn);
                fd = tmp.data();
                fn = tmp.size();
            }
        }

        std::string text = decode_text_frame(fd, fn);
        std::string* dst = f == Field::Title ? &tags.title : f == Field::Artist ? &tags.artist : &tags.album;
        if (dst->empty()) *dst = std::move(text);
    }
    return tags;
}

Id3Tags parse_id3v1(const uint8_t* data, size_t size)
{
    Id3Tags tags;
    if (!data || size < 128 || std::memcmp(data, "TAG", 3) != 0) return tags;
    auto field = [&](size_t off, size_t len) {
        size_t n = 0;
        while (n < len && data[off + n] != 0) ++n;
        std::string s = latin1_to_utf8(data + off, n);
        trim_trailing(s);
        return s;
    };
    tags.title = field(3, 30);
    tags.artist = field(33, 30);
    tags.album = field(63, 30);
    return tags;
}

Id3Tags read_id3v2_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    uint8_t header[10];
    if (!in.read(reinterpret_cast<char*>(header), 10)) return {};
    if (std::memcmp(header, "ID3", 3) != 0 || !is_syncsafe(header + 6)) return {};
    size_t body = std::min<size_t>(syncsafe32(header + 6), kMaxId3v2Read);
    std::vector<uint8_t> buf(10 + body);
    std::memcpy(buf.data(), header, 10);
    in.read(reinterpret_cast<char*>(buf.data() + 10), static_cast<std::streamsize>(body));
    size_t got = static_cast<size_t>(std::max<std::streamsize>(0, in.gcount()));
    return parse_id3v2(buf.data(), 10 + got);
}

Id3Tags read_id3v1_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    std::streamoff len = in.tellg();
    if (len < 128) return {};
    in.seekg(len - 128, std::ios::beg);
    uint8_t buf[128];
    if (!in.read(reinterpret_cast<char*>(buf), 128)) return {};
    return parse_id3v1(buf, 128);
}

} // namespace pyjuke
