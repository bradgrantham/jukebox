#include "font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace gfx
{

char32_t decode_utf8(std::string_view s, size_t& i)
{
    static constexpr char32_t REPLACEMENT = 0xFFFD;
    const auto byte = [&](size_t k) { return static_cast<uint8_t>(s[k]); };

    uint8_t b0 = byte(i);
    if(b0 < 0x80) {
        i += 1;
        return b0;
    }

    int length;
    char32_t cp;
    char32_t minimum;
    if((b0 & 0xE0) == 0xC0) {
        length = 2; cp = b0 & 0x1F; minimum = 0x80;
    } else if((b0 & 0xF0) == 0xE0) {
        length = 3; cp = b0 & 0x0F; minimum = 0x800;
    } else if((b0 & 0xF8) == 0xF0) {
        length = 4; cp = b0 & 0x07; minimum = 0x10000;
    } else {
        i += 1;
        return REPLACEMENT;
    }

    if(i + length > s.size()) {
        i += 1;
        return REPLACEMENT;
    }
    for(int k = 1; k < length; k++) {
        uint8_t b = byte(i + k);
        if((b & 0xC0) != 0x80) {
            i += 1;
            return REPLACEMENT;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    if(cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        i += 1;
        return REPLACEMENT;
    }
    i += length;
    return cp;
}

std::string default_font_path()
{
    static const char* candidates[] = {
#if defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Verdana Italic.ttf",
        "/Library/Fonts/Verdana Italic.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
#else
        // msttcorefonts (Debian/Ubuntu ttf-mscorefonts-installer, others)
        "/usr/share/fonts/truetype/msttcorefonts/Verdana_Italic.ttf",
        "/usr/share/fonts/truetype/msttcorefonts/verdanai.ttf",
        "/usr/share/fonts/msttcore/verdanai.ttf",
        "/usr/share/fonts/TTF/verdanai.ttf",
        "/usr/share/fonts/corefonts/verdanai.ttf",
        // DejaVu Sans Oblique, then upright
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
#endif
    };
    for(const char* path: candidates) {
        std::error_code ec;
        if(std::filesystem::is_regular_file(path, ec)) {
            return path;
        }
    }
    return "";
}

//----------------------------------------------------------------------------
// GlyphAtlas

GlyphAtlas::GlyphAtlas(int width, int height, int max_dim) :
    width_(width),
    height_(height),
    max_dim_(max_dim),
    pixels_(static_cast<size_t>(width) * height, 0)
{
}

bool GlyphAtlas::allocate(int w, int h, AtlasRegion& out)
{
    static constexpr int PAD = 1;
    for(;;) {
        // Fits on the current shelf?
        if(shelf_x_ + w + PAD <= width_ && shelf_y_ + h + PAD <= height_) {
            break;
        }
        // Fits on a new shelf below?
        int next_y = shelf_y_ + shelf_h_ + PAD;
        if(w + 2 * PAD <= width_ && next_y + h + PAD <= height_) {
            shelf_y_ = next_y;
            shelf_x_ = PAD;
            shelf_h_ = 0;
            break;
        }
        // Grow the smaller dimension, keeping every texel where it was so
        // existing regions stay valid.
        if(width_ <= height_ && width_ * 2 <= max_dim_) {
            int new_width = width_ * 2;
            std::vector<uint8_t> grown(static_cast<size_t>(new_width) * height_, 0);
            for(int row = 0; row < height_; row++) {
                memcpy(grown.data() + static_cast<size_t>(row) * new_width, pixels_.data() + static_cast<size_t>(row) * width_, width_);
            }
            pixels_.swap(grown);
            width_ = new_width;
        } else if(height_ * 2 <= max_dim_) {
            height_ *= 2;
            // Row-major with unchanged width, so existing rows stay put.
            pixels_.resize(static_cast<size_t>(width_) * height_, 0);
        } else {
            return false;
        }
        dirty_ = true;
    }
    out = AtlasRegion{shelf_x_, shelf_y_, w, h};
    shelf_x_ += w + PAD;
    shelf_h_ = std::max(shelf_h_, h);
    return true;
}

void GlyphAtlas::blit(const AtlasRegion& r, const uint8_t* src, int pitch)
{
    for(int row = 0; row < r.h; row++) {
        const uint8_t* s = src + static_cast<ptrdiff_t>(row) * pitch;
        uint8_t* d = pixels_.data() + static_cast<size_t>(r.y + row) * width_ + r.x;
        memcpy(d, s, r.w);
    }
    dirty_ = true;
}

void GlyphAtlas::clear()
{
    std::fill(pixels_.begin(), pixels_.end(), 0);
    shelf_x_ = 1;
    shelf_y_ = 1;
    shelf_h_ = 0;
    dirty_ = true;
}

//----------------------------------------------------------------------------
// FontFile

FontFile::FontFile(const std::string& path) :
    path_(path)
{
    std::ifstream in(path, std::ios::binary);
    if(!in) {
        throw std::runtime_error("couldn't open font file \"" + path + "\"");
    }
    bytes_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if(bytes_.empty()) {
        throw std::runtime_error("font file \"" + path + "\" is empty");
    }

    FT_Library library;
    if(FT_Init_FreeType(&library) != 0) {
        throw std::runtime_error("FT_Init_FreeType failed");
    }
    library_ = library;

    // Validate the file now so a bad path fails at startup.
    FT_Face face;
    if(FT_New_Memory_Face(library_, bytes_.data(), static_cast<FT_Long>(bytes_.size()), 0, &face) != 0) {
        FT_Done_FreeType(library_);
        library_ = nullptr;
        throw std::runtime_error("FreeType could not load font file \"" + path + "\"");
    }
    FT_Done_Face(face);
}

FontFile::~FontFile()
{
    if(library_ != nullptr) {
        FT_Done_FreeType(library_);
    }
}

//----------------------------------------------------------------------------
// Font

Font::Font(std::shared_ptr<const FontFile> file, float pixel_size, GlyphAtlas& atlas) :
    file_(std::move(file)),
    pixel_size_(pixel_size),
    atlas_(atlas)
{
    FT_Face face;
    if(FT_New_Memory_Face(file_->library_, file_->bytes_.data(), static_cast<FT_Long>(file_->bytes_.size()), 0, &face) != 0) {
        throw std::runtime_error("FreeType could not load font file \"" + file_->path() + "\"");
    }
    face_ = face;
    if(FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        std::cerr << "warning: font \"" << file_->path() << "\" has no Unicode charmap\n";
    }
    FT_F26Dot6 size26 = static_cast<FT_F26Dot6>(std::lround(std::max(1.0f, pixel_size) * 64.0f));
    if(FT_Set_Char_Size(face, 0, size26, 72, 72) != 0) {
        FT_Done_Face(face);
        face_ = nullptr;
        throw std::runtime_error("FreeType could not set size on font \"" + file_->path() + "\"");
    }
    ascent_ = static_cast<float>(face->size->metrics.ascender) / 64.0f;
    descent_ = -static_cast<float>(face->size->metrics.descender) / 64.0f;
    line_height_ = static_cast<float>(face->size->metrics.height) / 64.0f;
    has_kerning_ = FT_HAS_KERNING(face);

    for(char32_t cp = 0x20; cp <= 0x7E; cp++) {
        glyph(cp);
    }
    for(char32_t cp = 0xA0; cp <= 0xFF; cp++) {
        glyph(cp);
    }
}

Font::~Font()
{
    if(face_ != nullptr) {
        FT_Done_Face(face_);
    }
}

const Glyph& Font::glyph(char32_t codepoint)
{
    auto found = glyphs_.find(codepoint);
    if(found != glyphs_.end()) {
        return found->second;
    }
    Glyph& g = glyphs_[codepoint];
    bake(codepoint, g);
    return g;
}

void Font::bake(char32_t codepoint, Glyph& g)
{
    FT_Face face = face_;
    g.glyph_index = FT_Get_Char_Index(face, codepoint);  // 0 = .notdef box
    // Light hinting: vertical-only grid fitting, which keeps the italic's
    // shapes while giving crisp horizontal stems and whole-pixel advances.
    if(FT_Load_Glyph(face, g.glyph_index, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) != 0) {
        return;
    }
    if(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        return;
    }
    FT_GlyphSlot slot = face->glyph;
    g.advance = static_cast<int>((slot->advance.x + 32) >> 6);
    g.bearing_x = slot->bitmap_left;
    g.bearing_y = slot->bitmap_top;

    const FT_Bitmap& bm = slot->bitmap;
    if(bm.width == 0 || bm.rows == 0 || bm.pixel_mode != FT_PIXEL_MODE_GRAY) {
        return;
    }
    AtlasRegion r;
    if(!atlas_.allocate(static_cast<int>(bm.width), static_cast<int>(bm.rows), r)) {
        static bool warned = false;
        if(!warned) {
            std::cerr << "warning: glyph atlas is full; some glyphs will not be drawn\n";
            warned = true;
        }
        return;
    }
    atlas_.blit(r, bm.buffer, bm.pitch);
    g.region = r;
}

int Font::kerning(uint32_t left_glyph_index, uint32_t right_glyph_index) const
{
    if(!has_kerning_) {
        return 0;
    }
    FT_Vector delta;
    if(FT_Get_Kerning(face_, left_glyph_index, right_glyph_index, FT_KERNING_DEFAULT, &delta) != 0) {
        return 0;
    }
    return static_cast<int>(delta.x >> 6);
}

int Font::text_width(std::string_view utf8)
{
    return layout(utf8, [](const Glyph&, int) {});
}

};
