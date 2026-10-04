// FreeType glyph rasterisation into a single-channel (R8) atlas.
//
// All quantities in this header are in real framebuffer pixels; the Renderer
// converts to and from design units.  A GlyphAtlas is shared by any number of
// Font objects (one per pixel size), so a frame can mix text sizes while the
// GPU only ever binds one texture.
//
// No Vulkan here: the atlas is a CPU image with a dirty flag, and the Renderer
// uploads it when it changes.

#ifndef PYJUKE_FONT_H
#define PYJUKE_FONT_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct FT_LibraryRec_;
struct FT_FaceRec_;

namespace gfx
{

// Decode one UTF-8 code point starting at s[i] and advance i past it.
// Malformed or truncated sequences yield U+FFFD and advance by one byte.
char32_t decode_utf8(std::string_view s, size_t& i);

// Path of the platform default UI font, or "" if none of the candidates
// exists.  macOS: Verdana Italic from /System/Library/Fonts/Supplemental.
// Linux: Verdana Italic (msttcorefonts locations), then DejaVu Sans Oblique,
// then DejaVu Sans, in a few common distro locations.
std::string default_font_path();

// Rectangle within the atlas, in texels.  w == 0 or h == 0 means "nothing to
// draw" (e.g. a space).
struct AtlasRegion
{
    int x{0}, y{0}, w{0}, h{0};
};

// Shelf-packed R8 image.  Grows by doubling its smaller dimension (up to
// max_dim each way) when full; existing regions keep their texel
// coordinates.  Callers must treat every region as invalid after clear().
class GlyphAtlas
{
public:
    explicit GlyphAtlas(int width = 1024, int height = 512, int max_dim = 4096);

    // Reserve a w x h region (1 texel of padding is added around it).
    // Returns false if it cannot fit even at the maximum size.
    bool allocate(int w, int h, AtlasRegion& out);

    // Copy an 8-bit coverage bitmap into a previously allocated region.
    // pitch is the byte distance between rows of src (may be negative, as
    // FreeType allows).
    void blit(const AtlasRegion& r, const uint8_t* src, int pitch);

    // Forget all regions and zero the pixels; keeps the current size.
    void clear();

    // Set the largest width/height the atlas may grow to.
    void set_max_dim(int max_dim) { max_dim_ = max_dim; }

    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<uint8_t>& pixels() const { return pixels_; }

    // True if pixels changed since mark_clean().
    bool dirty() const { return dirty_; }
    void mark_clean() { dirty_ = false; }

private:
    int width_;
    int height_;
    int max_dim_;
    std::vector<uint8_t> pixels_;
    int shelf_x_{1};
    int shelf_y_{1};
    int shelf_h_{0};
    bool dirty_{true};
};

// A font file loaded into memory plus the FreeType library handle.  Shared by
// all Font objects made from it.  Throws std::runtime_error if the file
// cannot be read or FreeType rejects it.
class FontFile
{
public:
    explicit FontFile(const std::string& path);
    ~FontFile();
    FontFile(const FontFile&) = delete;
    FontFile& operator=(const FontFile&) = delete;

    const std::string& path() const { return path_; }

private:
    friend class Font;
    std::string path_;
    std::vector<uint8_t> bytes_;
    FT_LibraryRec_* library_{nullptr};
};

struct Glyph
{
    AtlasRegion region;     // where the coverage bitmap is in the atlas
    int bearing_x{0};       // pen position to left edge of bitmap
    int bearing_y{0};       // baseline to top edge of bitmap (positive = up)
    int advance{0};         // pen advance in whole pixels (hinted)
    uint32_t glyph_index{0};
};

// One face at one pixel size.  ASCII and Latin-1 (U+0020..U+00FF) are baked at
// construction; any other code point is rasterised the first time it is
// requested.  Glyph references stay valid until the Font is destroyed or the
// atlas is cleared.
class Font
{
public:
    Font(std::shared_ptr<const FontFile> file, float pixel_size, GlyphAtlas& atlas);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    float pixel_size() const { return pixel_size_; }

    // Metrics in pixels from the face's size metrics.  descent is positive
    // (distance below the baseline).  line_height is the font's recommended
    // baseline-to-baseline distance.
    float ascent() const { return ascent_; }
    float descent() const { return descent_; }
    float line_height() const { return line_height_; }

    const Glyph& glyph(char32_t codepoint);

    // Horizontal kerning adjustment in whole pixels between two glyph indices
    // (0 if the face has no 'kern' table).
    int kerning(uint32_t left_glyph_index, uint32_t right_glyph_index) const;

    // Walk a UTF-8 string, calling fn(glyph, pen_x) for each code point, where
    // pen_x is the pen position (pixels from the start of the string,
    // kerning applied) at which the glyph's origin sits.  Returns the total
    // advance width.  '\n' and other control characters are skipped.
    template <class F>
    int layout(std::string_view utf8, F&& fn)
    {
        int pen = 0;
        uint32_t previous = 0;
        size_t i = 0;
        while(i < utf8.size()) {
            char32_t cp = decode_utf8(utf8, i);
            if(cp < 0x20 || cp == 0x7f) {
                continue;
            }
            const Glyph& g = glyph(cp);
            if(previous != 0) {
                pen += kerning(previous, g.glyph_index);
            }
            fn(g, pen);
            pen += g.advance;
            previous = g.glyph_index;
        }
        return pen;
    }

    // Advance width of a UTF-8 string in pixels.
    int text_width(std::string_view utf8);

private:
    std::shared_ptr<const FontFile> file_;
    FT_FaceRec_* face_{nullptr};
    float pixel_size_;
    GlyphAtlas& atlas_;
    float ascent_{0}, descent_{0}, line_height_{0};
    bool has_kerning_{false};
    std::unordered_map<char32_t, Glyph> glyphs_;

    void bake(char32_t codepoint, Glyph& g);
};

};

#endif // PYJUKE_FONT_H
