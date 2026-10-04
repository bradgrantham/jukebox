// Resolution-independent 2D renderer for the pyjuke UI, on Vulkan.
//
// Coordinates
//   Every drawing call takes coordinates in a fixed *design space*
//   (design_w x design_h units, origin top left, y down; 800x480 for the
//   app).  Each frame the renderer computes one uniform scale
//       scale = min(fb_w / design_w, fb_h / design_h)
//   from the framebuffer size in real pixels, centres the scaled design area
//   in the framebuffer and fills the rest (letterbox/pillarbox) with black.
//   Nothing is drawn outside the design area.  HiDPI is handled because the
//   scale comes from the framebuffer size, not the window size.
//
// Text
//   Text is rasterised by FreeType at (size * scale) real pixels, so it is
//   crisp at any window size.  Glyphs are re-baked whenever the scale changes
//   (i.e. when the framebuffer is resized).  Several sizes may be used in one
//   frame.  Strings are UTF-8; ASCII and Latin-1 are baked up front and other
//   code points on first use.  Glyph origins are snapped to whole pixels.
//   Because glyph metrics are hinted at the real pixel size, text widths in
//   design units can differ by a fraction of a unit between window sizes;
//   lay out with text_width() each frame rather than caching it across
//   resizes.
//
// Colours
//   Color components are 0..1, straight (not premultiplied) alpha, and are
//   sRGB-encoded values (i.e. {0.5,0.5,0.5,1} is the same grey as #808080).
//   Blending is ordinary "over" compositing in that space.
//
// Frames
//   Drawing calls are only valid between begin_frame() returning true and
//   end_frame().  Calls draw in order (painter's algorithm).  The renderer
//   does not assume a continuous frame loop; draw whenever something changed
//   and on GLFW framebuffer-size / window-refresh events.
//
// Errors
//   The constructor throws std::runtime_error if the font cannot be found or
//   loaded (the message names the `font` config key); Vulkan failures print
//   the VkResult and exit, as elsewhere in vulkan_support.

#ifndef PYJUKE_RENDERER_H
#define PYJUKE_RENDERER_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct GLFWwindow;

namespace gfx
{

struct Color
{
    float r{0}, g{0}, b{0}, a{1};
};

// Axis-aligned rectangle in design units; (x, y) is the top left corner.
struct Rect
{
    float x{0}, y{0}, w{0}, h{0};
};

// An RGB8 image read back from the framebuffer (see Renderer::capture_next_frame).
struct Capture
{
    int width{0};
    int height{0};
    std::vector<uint8_t> rgb;   // width * height * 3 bytes, rows top to bottom
};

class Renderer
{
public:
    // window:     a GLFW window created with GLFW_CLIENT_API = GLFW_NO_API.
    //             glfwInit() must already have succeeded.
    // font_path:  TrueType/OpenType file; "" picks the platform default
    //             (see gfx::default_font_path() in font.h).
    // design_w/h: size of the design space in units.
    // validation: enable VK_LAYER_KHRONOS_validation and print its messages
    //             to stderr.
    Renderer(GLFWwindow* window, const std::string& font_path,
             float design_w, float design_h, bool validation);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Start a frame.  The design area is cleared to `clear`; the letterbox
    // bars are always black.  Returns false if nothing can be drawn now
    // (framebuffer has zero size, e.g. minimised); in that case do not call
    // any drawing functions or end_frame().  Recreates the swapchain if the
    // framebuffer size changed.
    bool begin_frame(Color clear);

    // Filled rectangle with corner radius `radius` (0 = square corners) and an
    // optional border `border_width` units wide drawn inside the rectangle's
    // edge in colour `border`.  Edges are antialiased.  The radius is clamped
    // to half the smaller side.
    void rounded_rect(Rect r, float radius, Color fill, float border_width = 0, Color border = {});

    // Draw a single line of UTF-8 text.  (x, y) is the left, top of the line
    // box; the baseline is at y + ascent(size).  `size` is the font pixel size
    // in design units (the old pygame font_size).
    void text(float x, float y, std::string_view utf8, float size, Color c);

    // Metrics in design units for text at `size`.  Usable outside a frame.
    float text_width(std::string_view utf8, float size);
    float line_height(float size);   // recommended baseline-to-baseline distance
    float ascent(float size);        // line top to baseline
    float descent(float size);       // baseline to line bottom (positive)

    // Restrict drawing to r (design units) intersected with the current clip.
    // Nestable; every push_clip() must be matched by pop_clip() within the
    // same frame.  Implemented with the scissor rect, so clipping is to whole
    // framebuffer pixels.
    void push_clip(Rect r);
    void pop_clip();

    // Submit the frame and present it.  Returns false if the swapchain turned
    // out to be out of date or suboptimal at present time (it has been
    // recreated); the caller should draw again soon.
    bool end_frame();

    // Convert GLFW cursor coordinates (window/screen units, as passed to the
    // cursor-position callback) to design units.  Points in the letterbox
    // map outside [0, design_w) x [0, design_h).
    void window_to_design(double wx, double wy, float& dx, float& dy) const;

    // Current mapping, for diagnostics: real pixels per design unit, and the
    // framebuffer-pixel position of the design area's top left corner.
    float scale() const;
    void design_origin(float& x, float& y) const;

    // Ask for the next frame's image to be read back.  After that frame's
    // end_frame(), take_capture() returns it.  Returns false (and captures
    // nothing) if the swapchain images cannot be used as transfer sources.
    bool capture_next_frame();
    bool take_capture(Capture& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

};

#endif // PYJUKE_RENDERER_H
