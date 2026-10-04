// gfx_demo: draws a static mock of the pyjuke screen with gfx::Renderer.
//
// usage: gfx_demo [--validation] [--frames N] [--size WxH] [--screenshot out.ppm]
//                 [--resize-test] [--font path]
//
//   --validation    enable VK_LAYER_KHRONOS_validation; messages go to stderr
//                   and the exit status is 3 if any were printed
//   --frames N      draw N frames continuously, then exit
//   --size WxH      initial window size in screen units (default 800x480)
//   --screenshot F  write the last frame (or the first, without --frames) to
//                   F as a binary PPM
//   --resize-test   with --frames, resize the window, iconify and restore it
//                   during the run to exercise swapchain recreation, ending at
//                   the --size size
//   --font path     font file instead of the platform default

#include <GLFW/glfw3.h>

#include "renderer.h"
#include "vulkan_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace
{

constexpr float DESIGN_W = 800;
constexpr float DESIGN_H = 480;

const gfx::Color white{1, 1, 1, 1};
const gfx::Color black{0, 0, 0, 1};
const gfx::Color red{1, 0.2f, 0.2f, 1};
const gfx::Color grey{0.55f, 0.55f, 0.55f, 1};
const gfx::Color dark_blue{0.05f, 0.08f, 0.2f, 1};

struct DemoState
{
    float mouse_x{-1}, mouse_y{-1};
    bool mouse_seen{false};
    bool dirty{true};
};

void usage(const char* prog)
{
    fprintf(stderr, "usage: %s [--validation] [--frames N] [--size WxH] [--screenshot out.ppm] [--resize-test] [--font path]\n", prog);
}

bool write_ppm(const std::string& path, const gfx::Capture& c)
{
    FILE* fp = fopen(path.c_str(), "wb");
    if(fp == nullptr) {
        perror(path.c_str());
        return false;
    }
    fprintf(fp, "P6\n%d %d\n255\n", c.width, c.height);
    size_t wrote = fwrite(c.rgb.data(), 1, c.rgb.size(), fp);
    fclose(fp);
    return wrote == c.rgb.size();
}

// A button in the style of uglypygui.RoundedButton: text with padding,
// rounded border; pressed buttons are drawn inverted.
float button(gfx::Renderer& r, float x, float y, float h, const char* label, float size, bool pressed)
{
    const float corner = 10, border = 3, pad = 14;
    float w = r.text_width(label, size) + 2 * pad;
    gfx::Color fg = pressed ? black : white;
    gfx::Color bg = pressed ? white : black;
    r.rounded_rect({x, y, w, h}, corner, bg, border, white);
    float ty = y + (h - r.line_height(size)) * 0.5f;
    r.text(x + pad, ty, label, size, fg);
    return w;
}

void draw(gfx::Renderer& r, const DemoState& state)
{
    const float big = 36, medium = 28, small = 22, tiny = 16;
    const float left = 20;

    // Track title (deliberately too long, clipped) and boxed track number.
    gfx::Rect title_box{left, 20, 600, 45};
    r.push_clip(title_box);
    r.text(title_box.x, title_box.y + (title_box.h - r.line_height(big)) * 0.5f,
           "Café del Mar — Über die Brücke Ω, a title much too long for its box", big, white);
    r.pop_clip();

    gfx::Rect number_box{645, 20, 135, 45};
    r.rounded_rect(number_box, 0, black, 3, white);
    const char* number = "12345";
    float nw = r.text_width(number, big);
    r.push_clip(number_box);
    r.text(number_box.x + number_box.w - 8 - nw, number_box.y + (number_box.h - r.line_height(big)) * 0.5f, number, big, white);
    r.pop_clip();

    r.text(left, 72, "Motörhead & Björk — Ça plane pour moi", medium, white);
    r.text(left, 108, "Album: Ünïcödé Tëst (naïve façade, € 9,99)", small, white);
    r.text(left, 140, "ASCII at 16: The quick brown fox jumps over the lazy dog 0123456789", tiny, grey);

    // Navigation row.
    const char* labels[] = {" |<- ", " << ", " || -> ", " >> ", " ->| ", "mode"};
    float x = left;
    float row_y = 190, row_h = 55;
    float widths[6];
    float total = 0;
    for(int i = 0; i < 6; i++) {
        widths[i] = r.text_width(labels[i], big) + 28;
        total += widths[i];
    }
    float gap = (760 - total) / 5;
    for(int i = 0; i < 6; i++) {
        button(r, x, row_y, row_h, labels[i], big, i == 2);
        x += widths[i] + gap;
    }

    // Time row: position | scrollbar | duration.
    float time_y = 270;
    r.text(left, time_y + (row_h - r.line_height(big)) * 0.5f, "1:23", big, white);
    gfx::Rect track{200, time_y, 400, row_h};
    r.rounded_rect(track, 10, black, 3, white);
    float spacing = 3, inner = 3 + spacing;
    float thumb_w = (track.w - 2 * inner) * 0.15f;
    float value = 0.3f;
    gfx::Rect thumb{track.x + inner + value * (track.w - 2 * inner - thumb_w), track.y + inner, thumb_w, track.h - 2 * inner};
    r.rounded_rect(thumb, 10 - inner + 3, white);
    const char* duration = "4:56";
    r.text(780 - r.text_width(duration, big), time_y + (row_h - r.line_height(big)) * 0.5f, duration, big, white);

    // Clock row.
    r.text(left, 345, "08:15 PM, Sat Oct 03", big, white);

    // Mixed sizes on one baseline (red line), to check baseline alignment.
    float baseline = 455;
    r.rounded_rect({left, baseline, 500, 1}, 0, red);
    float bx = left;
    const float sizes[] = {12, 16, 24, 36, 48};
    for(float s: sizes) {
        char label[32];
        snprintf(label, sizeof(label), "Ag%g ", s);
        r.text(bx, baseline - r.ascent(s), label, s, white);
        bx += r.text_width(label, s);
    }

    // Mouse position in design units, bottom right, in a translucent panel.
    char mouse[64];
    if(state.mouse_seen) {
        snprintf(mouse, sizeof(mouse), "mouse %.1f, %.1f", state.mouse_x, state.mouse_y);
    } else {
        snprintf(mouse, sizeof(mouse), "mouse: (move me)");
    }
    gfx::Rect panel{560, 410, 220, 60};
    r.rounded_rect(panel, 12, gfx::Color{0.2f, 0.4f, 1.0f, 0.5f}, 2, gfx::Color{1, 1, 1, 0.8f});
    r.text(panel.x + 10, panel.y + (panel.h - r.line_height(tiny)) * 0.5f, mouse, tiny, white);
}

}

int main(int argc, char** argv)
{
    bool validation = false;
    int frames = 0;
    int width = 800, height = 480;
    std::string screenshot;
    std::string font_path;
    bool resize_test = false;

    const char* prog = argv[0];
    for(int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> const char* {
            if(i + 1 >= argc) {
                usage(prog);
                fprintf(stderr, "%s requires %s\n", a.c_str(), what);
                exit(EXIT_FAILURE);
            }
            return argv[++i];
        };
        if(a == "--validation") {
            validation = true;
        } else if(a == "--frames") {
            frames = atoi(need("a frame count"));
        } else if(a == "--size") {
            const char* s = need("WxH");
            if(sscanf(s, "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
                fprintf(stderr, "bad --size \"%s\"; expected e.g. 1280x600\n", s);
                exit(EXIT_FAILURE);
            }
        } else if(a == "--screenshot") {
            screenshot = need("an output filename");
        } else if(a == "--resize-test") {
            resize_test = true;
        } else if(a == "--font") {
            font_path = need("a font file");
        } else {
            usage(prog);
            fprintf(stderr, "unknown option \"%s\"\n", a.c_str());
            exit(EXIT_FAILURE);
        }
    }

    glfwSetErrorCallback([](int, const char* description) { fprintf(stderr, "GLFW: %s\n", description); });
    if(!glfwInit()) {
        fprintf(stderr, "GLFW initialization failed\n");
        return EXIT_FAILURE;
    }
    if(!glfwVulkanSupported()) {
        fprintf(stderr, "GLFW reports Vulkan is not supported\n");
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(width, height, "gfx_demo", nullptr, nullptr);
    if(window == nullptr) {
        fprintf(stderr, "couldn't create window\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    int status = EXIT_SUCCESS;
    {
        std::unique_ptr<gfx::Renderer> renderer;
        try {
            renderer = std::make_unique<gfx::Renderer>(window, font_path, DESIGN_W, DESIGN_H, validation);
        } catch(const std::exception& e) {
            fprintf(stderr, "gfx_demo: %s\n", e.what());
            glfwDestroyWindow(window);
            glfwTerminate();
            return EXIT_FAILURE;
        }
        gfx::Renderer& r = *renderer;

        static DemoState state;
        static gfx::Renderer* rp = &r;
        glfwSetCursorPosCallback(window, [](GLFWwindow*, double x, double y) {
            rp->window_to_design(x, y, state.mouse_x, state.mouse_y);
            state.mouse_seen = true;
            state.dirty = true;
        });
        glfwSetFramebufferSizeCallback(window, [](GLFWwindow*, int, int) { state.dirty = true; });
        glfwSetWindowRefreshCallback(window, [](GLFWwindow*) { state.dirty = true; });
        glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int, int action, int) {
            if(action == GLFW_PRESS && (key == GLFW_KEY_Q || key == GLFW_KEY_ESCAPE)) {
                glfwSetWindowShouldClose(w, GLFW_TRUE);
            }
        });

        int drawn = 0;
        int iteration = 0;
        bool captured = false;
        while(!glfwWindowShouldClose(window)) {
            if(frames > 0) {
                glfwPollEvents();
                if(resize_test) {
                    // Scripted swapchain recreation: wider, taller, iconified,
                    // restored, and back to the requested size.
                    if(iteration == frames / 6) glfwSetWindowSize(window, width * 3 / 2, height);
                    if(iteration == 2 * frames / 6) glfwSetWindowSize(window, width / 2, height * 5 / 4);
                    if(iteration == 3 * frames / 6) glfwIconifyWindow(window);
                    if(iteration == 4 * frames / 6) glfwRestoreWindow(window);
                    if(iteration == 5 * frames / 6) glfwSetWindowSize(window, width, height);
                }
            } else {
                glfwWaitEvents();
                if(!state.dirty) {
                    continue;
                }
            }
            iteration++;

            if(frames > 0) {
                // No events needed: poll so scripted runs show something.
                double cx, cy;
                glfwGetCursorPos(window, &cx, &cy);
                r.window_to_design(cx, cy, state.mouse_x, state.mouse_y);
                state.mouse_seen = true;
            }

            bool last = (frames > 0) && (iteration >= frames);
            bool want_capture = !screenshot.empty() && !captured && (frames == 0 || last);
            if(want_capture && !r.capture_next_frame()) {
                fprintf(stderr, "swapchain images can't be read back; no screenshot\n");
                captured = true;
                status = EXIT_FAILURE;
            }

            if(r.begin_frame(dark_blue)) {
                draw(r, state);
                r.end_frame();
                drawn++;
                state.dirty = false;

                gfx::Capture capture;
                if(want_capture && r.take_capture(capture)) {
                    if(write_ppm(screenshot, capture)) {
                        printf("wrote %s (%dx%d framebuffer pixels, scale %.3f)\n", screenshot.c_str(), capture.width, capture.height, r.scale());
                    } else {
                        status = EXIT_FAILURE;
                    }
                    captured = true;
                }
            }

            if(last) {
                break;
            }
        }

        if(frames > 0) {
            printf("drew %d of %d frames\n", drawn, frames);
            int ww, wh;
            glfwGetWindowSize(window, &ww, &wh);
            float x0, y0, x1, y1, xc, yc;
            r.window_to_design(0, 0, x0, y0);
            r.window_to_design(ww, wh, x1, y1);
            r.window_to_design(ww / 2.0, wh / 2.0, xc, yc);
            printf("window %dx%d: corners map to design (%.1f, %.1f) and (%.1f, %.1f), centre to (%.1f, %.1f)\n",
                   ww, wh, x0, y0, x1, y1, xc, yc);
        }
        if(!screenshot.empty() && !captured) {
            fprintf(stderr, "no frame was captured for --screenshot\n");
            status = EXIT_FAILURE;
        }
        renderer.reset();
    }

    if(validation) {
        uint32_t count = VulkanSupport::ValidationMessageCount();
        printf("validation messages: %u\n", count);
        if(count > 0) {
            status = 3;
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return status;
}
