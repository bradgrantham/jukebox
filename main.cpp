// pyjuke - MP3 jukebox.  C++ port of pyjuke.py: minimp3 for decoding, libao
// for output, GLFW + Vulkan for the display.
//
// usage: pyjuke [options] [musicdir]
//   --config FILE      read FILE instead of ~/.pyjuke/config
//   --validation       enable the Vulkan validation layer
//   --exit-after SECS  quit after SECS seconds (for scripted runs)
//   --screenshot FILE  write the last frame drawn to FILE (PPM) on exit

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>

#include "audio.h"
#include "config.h"
#include "library.h"
#include "player.h"
#include "renderer.h"
#include "ui.h"

namespace
{

constexpr float design_width = 800;
constexpr float design_height = 480;

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The player screen, laid out as init_gui() in pyjuke.py did.
struct JukeboxScreen
{
    std::vector<std::unique_ptr<ui::Widget>> widgets;

    ui::BoxedText* track_number;
    ui::BoxedText* track_name;
    ui::BoxedText* track_artist;
    ui::BoxedText* track_album;
    ui::BoxedText* last_line;
    ui::Text* mode_text;
    ui::BoxedText* position_text;
    ui::BoxedText* duration_text;
    ui::Scrollbar* time_scrollbar;
    ui::BoxedText* date_time_text;
    std::unique_ptr<ui::Screen> screen;

    template <class T, class... Args>
    T* make(Args&&... args)
    {
        auto widget = std::make_unique<T>(std::forward<Args>(args)...);
        T* p = widget.get();
        widgets.push_back(std::move(widget));
        return p;
    }

    JukeboxScreen(gfx::Renderer& r, pyjuke::Player& player, float font, const std::function<int64_t()>& now)
    {
        using namespace ui;

        const float button_corner = 10;
        const float button_border = 3;

        track_number = make<BoxedText>(120.0f, 45.0f, "", font, white, black, Justify::Right);
        auto track_bordered = make<Bordered>(track_number);
        track_name = make<BoxedText>(600.0f, 45.0f, "", font, white, black, Justify::Left);
        auto number_name_hbox = make<HBox>(760.0f, 45.0f, std::vector<Widget*>{track_name, make<Spacer>(25.0f, 1.0f), track_bordered}, Justify::Left);

        track_artist = make<BoxedText>(760.0f, 36.0f, "", font, white, black, Justify::Left);
        track_album = make<BoxedText>(760.0f, 36.0f, "", font, white, black, Justify::Left);
        last_line = make<BoxedText>(760.0f, 36.0f, "", font, white, black, Justify::Left);

        auto track_info_vbox = make<VBox>(760.0f, 45.0f * 4, std::vector<Widget*>{number_name_hbox, make<Spacer>(1.0f, 5.0f), track_artist, track_album, last_line}, Justify::Distribute, 1.0f, Justify::Left);

        auto button = [&](Widget* child) {
            return make<RoundedButton>(button_border, button_corner, white, black, child);
        };

        auto previous_button = button(make<Text>("\\<-", font));
        previous_button->on_release_inside = [&player, now] { player.prev(now()); };

        auto rewind_button = button(make<Text>("<<", font));
        rewind_button->on_press = [&player, now] { player.seek_button_press(pyjuke::SeekDir::Backward, now()); };
        rewind_button->on_release_inside = [&player, now] { player.seek_button_release(pyjuke::SeekDir::Backward, now()); };
        rewind_button->on_release_outside = rewind_button->on_release_inside;

        auto play_pause_button = button(make<Text>("\\\\ ->", font));
        play_pause_button->on_release_inside = [&player] { player.toggle_play(); };

        auto forward_button = button(make<Text>(">>", font));
        forward_button->on_press = [&player, now] { player.seek_button_press(pyjuke::SeekDir::Forward, now()); };
        forward_button->on_release_inside = [&player, now] { player.seek_button_release(pyjuke::SeekDir::Forward, now()); };
        forward_button->on_release_outside = forward_button->on_release_inside;

        auto next_button = button(make<Text>("->\\", font));
        next_button->on_release_inside = [&player, now] { player.next(now()); };

        mode_text = make<Text>("mode", font);
        auto mode_button = button(make<HBox>(240.0f, 40.0f, std::vector<Widget*>{mode_text}, Justify::Distribute));
        mode_button->on_release_inside = [&player] { player.change_mode(); };

        auto nav_panel = make<HBox>(760.0f, 55.0f, std::vector<Widget*>{previous_button, rewind_button, play_pause_button, forward_button, next_button, mode_button}, Justify::Distribute);

        position_text = make<BoxedText>(150.0f, 36.0f, "", font);
        duration_text = make<BoxedText>(150.0f, 36.0f, "", font);

        // The time row is as tall as a button.
        next_button->layout(r);
        float button_height = next_button->height;

        Scrollbar::Style style;
        style.border_width = button_border;
        style.corner_radius = button_corner;
        style.thumb_spacing = 3;
        style.thumb_fraction = .15f;
        time_scrollbar = make<Scrollbar>(400.0f, button_height, 0.0f, 1000.0f, style);
        time_scrollbar->on_scroll_continue = [&player](float value) { player.scroll_seek(static_cast<int>(value)); };
        time_scrollbar->on_scroll_finish = time_scrollbar->on_scroll_continue;
        auto time_hbox = make<HBox>(760.0f, button_height, std::vector<Widget*>{position_text, time_scrollbar, duration_text}, Justify::Distribute);

        date_time_text = make<BoxedText>(460.0f, 36.0f, "", font, white, black, Justify::Left);
        auto date_time_hbox = make<HBox>(760.0f, 50.0f, std::vector<Widget*>{date_time_text}, Justify::Left);

        auto pane = make<VBox>(760.0f, 460.0f, std::vector<Widget*>{track_info_vbox, make<Spacer>(1.0f, 25.0f), nav_panel, make<Spacer>(1.0f, 25.0f), time_hbox, make<Spacer>(1.0f, 25.0f), date_time_hbox}, Justify::Top);

        screen = std::make_unique<Screen>(pane, 20.0f, 20.0f);
    }

    void update(const pyjuke::Player& player)
    {
        const pyjuke::TrackInfo& info = player.info();
        track_name->set_text(info.title);
        track_artist->set_text(info.artist);
        track_album->set_text(info.album);
        track_number->set_text(std::to_string(player.current_index()));
        last_line->set_text(player.stopped_on_errors() ? "Stopped: could not play any track" : player.state_name());
        mode_text->set_text(player.mode_name());
        position_text->set_text(pyjuke::format_time(player.position_ms()));
        duration_text->set_text(pyjuke::format_time(player.duration_ms()));
        int scroll = player.scroll_value();
        if(!time_scrollbar->is_grabbed() && scroll >= 0) {
            time_scrollbar->set_value(static_cast<float>(scroll));
        }

        char clock[64];
        std::time_t t = std::time(nullptr);
        std::strftime(clock, sizeof(clock), "%I:%M %p, %a %b %d", std::localtime(&t));
        date_time_text->set_text(clock);
    }
};

struct App
{
    gfx::Renderer* renderer = nullptr;
    pyjuke::Player* player = nullptr;
    JukeboxScreen* screen = nullptr;
    bool dirty = true;
};

App& app_of(GLFWwindow* window)
{
    return *static_cast<App*>(glfwGetWindowUserPointer(window));
}

std::optional<pyjuke::SeekDir> seek_key(int key)
{
    switch(key) {
        case GLFW_KEY_PAGE_DOWN: case GLFW_KEY_X: case GLFW_KEY_KP_6:
            return pyjuke::SeekDir::Forward;
        case GLFW_KEY_PAGE_UP: case GLFW_KEY_Z: case GLFW_KEY_KP_4:
            return pyjuke::SeekDir::Backward;
    }
    return std::nullopt;
}

void key_callback(GLFWwindow* window, int key, int, int action, int)
{
    App& app = app_of(window);
    app.dirty = true;

    if(key == GLFW_KEY_Q && action == GLFW_RELEASE) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        return;
    }
    if(app.player == nullptr || action == GLFW_REPEAT) {
        return;
    }
    pyjuke::Player& player = *app.player;

    if(auto dir = seek_key(key)) {
        if(action == GLFW_PRESS) {
            player.seek_key_press(*dir, now_ms());
        } else {
            player.seek_key_release(*dir, now_ms());
        }
        return;
    }

    if(action != GLFW_RELEASE) {
        return;
    }
    switch(key) {
        case GLFW_KEY_F5: case GLFW_KEY_ESCAPE: case GLFW_KEY_C: case GLFW_KEY_SPACE: case GLFW_KEY_KP_5:
            player.toggle_play();
            break;
        case GLFW_KEY_B: case GLFW_KEY_KP_ENTER: case GLFW_KEY_KP_8:
            player.change_mode();
            break;
    }
}

void mouse_event(GLFWwindow* window, ui::Mouse what)
{
    App& app = app_of(window);
    if(app.screen == nullptr) {
        return;
    }
    double wx, wy;
    glfwGetCursorPos(window, &wx, &wy);
    float x, y;
    app.renderer->window_to_design(wx, wy, x, y);
    app.screen->screen->mouse_event(what, x, y);
    app.dirty = true;
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int)
{
    if(button == GLFW_MOUSE_BUTTON_LEFT) {
        mouse_event(window, (action == GLFW_PRESS) ? ui::Mouse::Press : ui::Mouse::Release);
    }
}

void cursor_pos_callback(GLFWwindow* window, double, double)
{
    mouse_event(window, ui::Mouse::Motion);
}

bool write_ppm(const std::string& path, const gfx::Capture& capture)
{
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << capture.width << " " << capture.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(capture.rgb.data()), capture.rgb.size());
    return static_cast<bool>(out);
}

void usage(const char* progname)
{
    fprintf(stderr, "usage: %s [--config FILE] [--validation] [--exit-after SECS] [--screenshot FILE] [musicdir]\n", progname);
}

int run(GLFWwindow* window, const pyjuke::Config& config, bool validation, double exit_after, const std::string& screenshot)
{
    std::unique_ptr<gfx::Renderer> renderer;
    try {
        renderer = std::make_unique<gfx::Renderer>(window, config.font, design_width, design_height, validation);
    } catch(const std::runtime_error& e) {
        fprintf(stderr, "%s\n", e.what());
        return EXIT_FAILURE;
    }
    gfx::Renderer& r = *renderer;

    App app;
    app.renderer = &r;
    glfwSetWindowUserPointer(window, &app);
    glfwSetKeyCallback(window, key_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_pos_callback);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int, int) { app_of(w).dirty = true; });
    glfwSetWindowRefreshCallback(window, [](GLFWwindow* w) { app_of(w).dirty = true; });

    const float font = static_cast<float>(config.font_size);
    const int64_t start_time = now_ms();
    const int64_t exit_time = (exit_after > 0) ? start_time + static_cast<int64_t>(exit_after * 1000) : -1;

    // Draws one frame through draw_contents; on the way out, captures it too.
    bool capturing = false;
    auto draw_frame = [&](const std::function<void()>& draw_contents) {
        if(capturing) {
            r.capture_next_frame();
        }
        if(!r.begin_frame(ui::black)) {
            return;
        }
        draw_contents();
        app.dirty = !r.end_frame();
    };

    // Populate the playlist, showing progress as the old one did.
    std::vector<std::filesystem::path> tracks;
    {
        pyjuke::ScanProgress progress;
        std::thread scanner([&] {
            tracks = pyjuke::scan_library(config.musicdir, &progress);
            glfwPostEmptyEvent();
        });
        while(!progress.done) {
            draw_frame([&] {
                float line = r.line_height(font);
                r.text(20, 20, "Populating Playlist...", font, ui::white);
                r.text(20, 20 + line, "Have read " + std::to_string(progress.entries_seen.load()) + " files so far", font, ui::white);
            });
            glfwWaitEventsTimeout(.1);
            if(glfwWindowShouldClose(window)) {
                progress.cancel = true;
            }
        }
        scanner.join();
    }

    if(tracks.empty() && !glfwWindowShouldClose(window)) {
        fprintf(stderr, "no MP3 files found under \"%s\"\n", config.musicdir.c_str());
        while(!glfwWindowShouldClose(window) && (exit_time < 0 || now_ms() < exit_time)) {
            draw_frame([&] {
                r.text(20, 20, "No MP3 files found in", font, ui::white);
                r.text(20, 20 + r.line_height(font), config.musicdir, font, ui::white);
            });
            glfwWaitEventsTimeout(.5);
        }
        return EXIT_FAILURE;
    }

    if(!glfwWindowShouldClose(window)) {
        // The engine goes away before the caller terminates GLFW, which its wake callback uses.
        pyjuke::AudioEngine audio([] { glfwPostEmptyEvent(); });

        pyjuke::PlayerTiming timing;
        timing.seek_wait_ms = config.seek_wait_millis;
        timing.seek_repeat_ms = config.seek_repeat_millis;
        timing.seek_delta_ms = config.seek_delta_millis;
        timing.reverse_skip_ms = config.reverse_skip_millis;

        pyjuke::Player player(audio, std::move(tracks), config.musicdir, timing, std::random_device{}());
        JukeboxScreen screen(r, player, font, now_ms);
        app.player = &player;
        app.screen = &screen;

        player.start(now_ms());

        int64_t next_status = 0;
        int64_t next_clock = 0;
        bool exiting = false;

        while(!glfwWindowShouldClose(window) && !exiting) {
            int64_t now = now_ms();

            if(player.tick(now)) {
                app.dirty = true;
            }
            if(now >= next_status) {
                next_status = now + config.status_repeat_millis;
                app.dirty = true;
            }
            if(now >= next_clock) {
                next_clock = now + config.clock_repeat_millis;
                app.dirty = true;
            }
            if(exit_time >= 0 && now >= exit_time) {
                exiting = true;
                capturing = !screenshot.empty();
                app.dirty = true;
            }

            if(app.dirty) {
                screen.update(player);
                draw_frame([&] { screen.screen->draw(r); });
            }

            if(app.dirty) {
                // The swapchain was rebuilt under us; draw again right away.
                glfwPollEvents();
                continue;
            }

            int64_t deadline = std::min(next_status, next_clock);
            if(auto player_deadline = player.next_deadline()) {
                deadline = std::min(deadline, *player_deadline);
            }
            if(exit_time >= 0) {
                deadline = std::min(deadline, exit_time);
            }
            glfwWaitEventsTimeout(std::max<int64_t>(deadline - now_ms(), 1) / 1000.0);
        }

        app.player = nullptr;
        app.screen = nullptr;

        if(capturing) {
            gfx::Capture capture;
            if(!r.take_capture(capture) || !write_ppm(screenshot, capture)) {
                fprintf(stderr, "couldn't write screenshot to \"%s\"\n", screenshot.c_str());
                return EXIT_FAILURE;
            }
        }
    }

    return EXIT_SUCCESS;
}

};

int main(int argc, char** argv)
{
    std::filesystem::path config_path = pyjuke::default_config_path();
    std::string musicdir;
    std::string screenshot;
    bool validation = false;
    double exit_after = 0;

    for(int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        bool has_value = i + 1 < argc;
        if(arg == "--config" && has_value) {
            config_path = argv[++i];
        } else if(arg == "--validation") {
            validation = true;
        } else if(arg == "--exit-after" && has_value) {
            exit_after = atof(argv[++i]);
        } else if(arg == "--screenshot" && has_value) {
            screenshot = argv[++i];
        } else if(arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if(arg[0] == '-' || !musicdir.empty()) {
            usage(argv[0]);
            return EXIT_FAILURE;
        } else {
            musicdir = arg;
        }
    }

    pyjuke::Config config = pyjuke::load_config(config_path);
    if(!musicdir.empty()) {
        config.musicdir = musicdir;
    }

    glfwSetErrorCallback([](int, const char* description) { fprintf(stderr, "GLFW: %s\n", description); });
    if(!glfwInit()) {
        fprintf(stderr, "couldn't initialize GLFW\n");
        return EXIT_FAILURE;
    }
    if(!glfwVulkanSupported()) {
        fprintf(stderr, "GLFW found no Vulkan loader\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWmonitor* monitor = nullptr;
    int width = config.width;
    int height = config.height;
    if(config.fullscreen) {
        monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        width = mode->width;
        height = mode->height;
    }
    GLFWwindow* window = glfwCreateWindow(width, height, "pyjuke", monitor, nullptr);
    if(window == nullptr) {
        fprintf(stderr, "couldn't create a window\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    int result = run(window, config, validation, exit_after, screenshot);

    glfwDestroyWindow(window);
    glfwTerminate();
    return result;
}
