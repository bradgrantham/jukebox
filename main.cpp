// pyjuke - MP3 jukebox.  C++ port of pyjuke.py: minimp3 for decoding, libao
// for output, GLFW + Vulkan for the display.
//
// usage: pyjuke [options] [musicdir]
//   --config FILE      read FILE instead of ~/.pyjuke/config
//   --validation       enable the Vulkan validation layer
//   --exit-after SECS  quit after SECS seconds (for scripted runs)
//   --screenshot FILE  write the last frame drawn to FILE (PPM) on exit
//
// Output picker: the button at the bottom right names where the sound goes,
// this computer or a UPnP renderer found on the LAN; pressing it drops a list
// up over the controls. The O key cycles through the outputs.

#include <algorithm>
#include <cctype>
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
#include "audio_switch.h"
#include "config.h"
#include "library.h"
#include "net.h"
#include "player.h"
#include "remote_renderer.h"
#include "renderer.h"
#include "track_server.h"
#include "ui.h"
#include "upnp.h"

namespace
{

constexpr float design_width = 800;
constexpr float design_height = 480;

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool contains_ci(const std::string& text, const std::string& needle)
{
    auto lower = [](std::string v) {
        for(char& c: v) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return v;
    };
    return lower(text).find(lower(needle)) != std::string::npos;
}

// Where the sound goes: the local AudioEngine or a UPnP renderer on the LAN.
// The Player drives `audio`, an AudioSwitch, and never learns which is behind
// it; moving playback is "point the switch elsewhere, reload the track there".
struct Outputs
{
    static constexpr const char* local_name = "This computer";

    Outputs(pyjuke::AudioEngine& local_, pyjuke::AudioSwitch& audio_, pyjuke::TrackServer* server_, pyjuke::upnp::RendererBrowser* browser_, std::filesystem::path musicdir_, std::string default_output_) :
        local(local_),
        audio(audio_),
        server(server_),
        browser(browser_),
        musicdir(std::move(musicdir_)),
        default_output(std::move(default_output_))
    {
    }

    pyjuke::AudioEngine& local;
    pyjuke::AudioSwitch& audio;
    pyjuke::TrackServer* server;             // null when network outputs are off
    pyjuke::upnp::RendererBrowser* browser;  // likewise
    std::filesystem::path musicdir;
    std::string default_output;              // config: switch to this renderer when it appears

    std::unique_ptr<pyjuke::RemoteRenderer> remote;  // the current remote output, if any
    std::string notice;                       // status-line message until notice_until
    int64_t notice_until = 0;
    int64_t next_refresh = 0;
    bool chosen_by_hand = false;              // a manual pick overrides default_output
    bool default_applied = false;

    bool enabled() const { return server != nullptr && browser != nullptr; }
    std::string current_udn() const { return remote ? remote->target().udn : std::string(); }
    std::string current_name() const { return remote ? remote->target().name : std::string(local_name); }
    std::vector<pyjuke::upnp::Renderer> renderers() const
    {
        return browser != nullptr ? browser->renderers() : std::vector<pyjuke::upnp::Renderer>{};
    }

    void say(std::string text, int64_t now)
    {
        notice = std::move(text);
        notice_until = now + 8000;
    }

    // Move playback to `target`, or to this computer when null.
    void select(const pyjuke::upnp::Renderer* target, pyjuke::Player& player, int64_t now)
    {
        if(target == nullptr && !remote) {
            return;
        }
        if(target != nullptr && remote && remote->target().udn == target->udn) {
            return;
        }
        int64_t position = std::max<int64_t>(player.position_ms(), 0);
        // Quiet the output being left. A RemoteRenderer sends Stop as it goes.
        audio.set_target(local);
        if(remote) {
            remote.reset();
        } else {
            local.pause();
        }
        if(target != nullptr) {
            std::string host = pyjuke::net::local_address_for(target->host);
            if(host.empty()) {
                say("No route to " + target->name + "; playing here", now);
            } else {
                remote = std::make_unique<pyjuke::RemoteRenderer>(*target, *server, host, musicdir, [] { glfwPostEmptyEvent(); });
                audio.set_target(*remote);
            }
        }
        player.reload_current(position);
    }

    // Keyboard: the next output in picker order (this computer first).
    void cycle(pyjuke::Player& player, int64_t now)
    {
        std::vector<pyjuke::upnp::Renderer> list = renderers();
        if(list.empty()) {
            return;
        }
        size_t current = 0;  // 0 = local, k + 1 = list[k]
        std::string udn = current_udn();
        for(size_t k = 0; k < list.size(); k++) {
            if(list[k].udn == udn) {
                current = k + 1;
            }
        }
        size_t next = (current + 1) % (list.size() + 1);
        chosen_by_hand = true;
        select(next == 0 ? nullptr : &list[next - 1], player, now);
    }

    // Per-frame upkeep: a lost renderer, periodic searches, the configured
    // default. Returns true if what the screen shows may have changed.
    bool tick(pyjuke::Player& player, int64_t now)
    {
        bool changed = false;
        if(remote && remote->lost()) {
            std::string name = remote->target().name;
            select(nullptr, player, now);
            say(name + " stopped answering; playing here", now);
            changed = true;
        }
        if(!notice.empty() && now >= notice_until) {
            notice.clear();
            changed = true;
        }
        if(!enabled()) {
            return changed;
        }
        if(now >= next_refresh) {
            browser->refresh();
            next_refresh = now + 60 * 1000;
        }
        if(!default_applied && !chosen_by_hand && !default_output.empty()) {
            for(const pyjuke::upnp::Renderer& r: browser->renderers()) {
                if(contains_ci(r.name, default_output)) {
                    default_applied = true;
                    select(&r, player, now);
                    changed = true;
                    break;
                }
            }
        }
        return changed;
    }
};

// The player screen, laid out as init_gui() in pyjuke.py did, plus the
// output picker: a modal layer that update() keeps in step with picker_open
// and the renderer list. Callbacks only change state; the tree changes here.
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
    ui::BoxedText* output_text;
    std::unique_ptr<ui::Screen> screen;

    gfx::Renderer& renderer;
    Outputs& outputs;
    float font;
    float button_height = 0;
    static constexpr float button_corner = 10;
    static constexpr float button_border = 3;
    static constexpr size_t max_picker_rows = 5;

    bool picker_open = false;
    bool picker_shown = false;        // the overlay layer is pushed
    uint64_t picker_generation = 0;   // renderer list the rows were built from
    std::string picker_current;       // output marked when the rows were built
    bool picker_searching = false;
    std::vector<std::unique_ptr<ui::Widget>> picker_widgets;
    ui::Widget* picker_root = nullptr;

    // Widgets are owned here; the tree only borrows pointers.
    template <class T, class... Args>
    static T* make_owned(std::vector<std::unique_ptr<ui::Widget>>& owner, Args&&... args)
    {
        auto widget = std::make_unique<T>(std::forward<Args>(args)...);
        T* p = widget.get();
        owner.push_back(std::move(widget));
        return p;
    }

    JukeboxScreen(gfx::Renderer& r, pyjuke::Player& player, Outputs& outputs_, float font_, const std::function<int64_t()>& now) :
        renderer(r),
        outputs(outputs_),
        font(font_)
    {
        using namespace ui;

        track_number = make_owned<BoxedText>(widgets, 120.0f, 45.0f, "", font, white, black, Justify::Right);
        auto track_bordered = make_owned<Bordered>(widgets, track_number);
        track_name = make_owned<BoxedText>(widgets, 600.0f, 45.0f, "", font, white, black, Justify::Left);
        auto number_name_hbox = make_owned<HBox>(widgets, 760.0f, 45.0f, std::vector<Widget*>{track_name, make_owned<Spacer>(widgets, 25.0f, 1.0f), track_bordered}, Justify::Left);

        track_artist = make_owned<BoxedText>(widgets, 760.0f, 36.0f, "", font, white, black, Justify::Left);
        track_album = make_owned<BoxedText>(widgets, 760.0f, 36.0f, "", font, white, black, Justify::Left);
        last_line = make_owned<BoxedText>(widgets, 760.0f, 36.0f, "", font, white, black, Justify::Left);

        auto track_info_vbox = make_owned<VBox>(widgets, 760.0f, 45.0f * 4, std::vector<Widget*>{number_name_hbox, make_owned<Spacer>(widgets, 1.0f, 5.0f), track_artist, track_album, last_line}, Justify::Distribute, 1.0f, Justify::Left);

        auto button = [&](Widget* child) {
            return make_owned<RoundedButton>(widgets, button_border, button_corner, white, black, child);
        };

        auto previous_button = button(make_owned<Text>(widgets, "\\<-", font));
        previous_button->on_release_inside = [&player, now] { player.prev(now()); };

        auto rewind_button = button(make_owned<Text>(widgets, "<<", font));
        rewind_button->on_press = [&player, now] { player.seek_button_press(pyjuke::SeekDir::Backward, now()); };
        rewind_button->on_release_inside = [&player, now] { player.seek_button_release(pyjuke::SeekDir::Backward, now()); };
        rewind_button->on_release_outside = rewind_button->on_release_inside;

        auto play_pause_button = button(make_owned<Text>(widgets, "\\\\ ->", font));
        play_pause_button->on_release_inside = [&player] { player.toggle_play(); };

        auto forward_button = button(make_owned<Text>(widgets, ">>", font));
        forward_button->on_press = [&player, now] { player.seek_button_press(pyjuke::SeekDir::Forward, now()); };
        forward_button->on_release_inside = [&player, now] { player.seek_button_release(pyjuke::SeekDir::Forward, now()); };
        forward_button->on_release_outside = forward_button->on_release_inside;

        auto next_button = button(make_owned<Text>(widgets, "->\\", font));
        next_button->on_release_inside = [&player, now] { player.next(now()); };

        mode_text = make_owned<Text>(widgets, "mode", font);
        auto mode_button = button(make_owned<HBox>(widgets, 240.0f, 40.0f, std::vector<Widget*>{mode_text}, Justify::Distribute));
        mode_button->on_release_inside = [&player] { player.change_mode(); };

        auto nav_panel = make_owned<HBox>(widgets, 760.0f, 55.0f, std::vector<Widget*>{previous_button, rewind_button, play_pause_button, forward_button, next_button, mode_button}, Justify::Distribute);

        position_text = make_owned<BoxedText>(widgets, 150.0f, 36.0f, "", font);
        duration_text = make_owned<BoxedText>(widgets, 150.0f, 36.0f, "", font);

        // The time row and the clock row are as tall as a button.
        next_button->layout(r);
        button_height = next_button->height;

        Scrollbar::Style style;
        style.border_width = button_border;
        style.corner_radius = button_corner;
        style.thumb_spacing = 3;
        style.thumb_fraction = .15f;
        time_scrollbar = make_owned<Scrollbar>(widgets, 400.0f, button_height, 0.0f, 1000.0f, style);
        time_scrollbar->on_scroll_continue = [&player](float value) { player.scroll_seek(static_cast<int>(value)); };
        time_scrollbar->on_scroll_finish = time_scrollbar->on_scroll_continue;
        auto time_hbox = make_owned<HBox>(widgets, 760.0f, button_height, std::vector<Widget*>{position_text, time_scrollbar, duration_text}, Justify::Distribute);

        // Clock on the left, the output button flush right.
        output_text = make_owned<BoxedText>(widgets, 240.0f, 40.0f, "", font, white, black, Justify::Left);
        auto output_button = button(output_text);
        output_button->layout(r);
        output_button->on_release_inside = [this] {
            picker_open = !picker_open;
            if(picker_open && outputs.browser != nullptr) {
                outputs.browser->refresh();
            }
        };
        date_time_text = make_owned<BoxedText>(widgets, 760.0f - output_button->width - 4, 36.0f, "", font, white, black, Justify::Left);
        auto date_time_hbox = make_owned<HBox>(widgets, 760.0f, button_height, std::vector<Widget*>{date_time_text, output_button}, Justify::Left);

        auto pane = make_owned<VBox>(widgets, 760.0f, 460.0f, std::vector<Widget*>{track_info_vbox, make_owned<Spacer>(widgets, 1.0f, 25.0f), nav_panel, make_owned<Spacer>(widgets, 1.0f, 25.0f), time_hbox, make_owned<Spacer>(widgets, 1.0f, 25.0f), date_time_hbox}, Justify::Top);

        screen = std::make_unique<Screen>(pane, 20.0f, 20.0f);
    }

    // One row per output, this computer first, the current one starred;
    // right-aligned with the pane and sitting just above the clock row.
    void rebuild_picker(pyjuke::Player& player, const std::function<int64_t()>& now)
    {
        using namespace ui;
        picker_widgets.clear();
        picker_root = nullptr;

        std::vector<Widget*> rows;
        auto row = [&](const std::string& label, std::function<void()> action) {
            auto text = make_owned<BoxedText>(picker_widgets, 240.0f, 40.0f, label, font, white, black, Justify::Left);
            auto b = make_owned<RoundedButton>(picker_widgets, button_border, button_corner, white, black, text);
            b->on_release_inside = std::move(action);
            rows.push_back(b);
        };
        std::string current = outputs.current_udn();
        auto mark = [&](bool selected) { return selected ? "* " : "  "; };
        row(mark(current.empty()) + std::string(Outputs::local_name), [this, &player, now] {
            outputs.chosen_by_hand = true;
            outputs.select(nullptr, player, now());
            picker_open = false;
        });
        std::vector<pyjuke::upnp::Renderer> list = outputs.renderers();
        for(size_t i = 0; i < list.size() && rows.size() < max_picker_rows; i++) {
            pyjuke::upnp::Renderer r = list[i];
            row(mark(r.udn == current) + r.name, [this, &player, now, r] {
                outputs.chosen_by_hand = true;
                outputs.select(&r, player, now());
                picker_open = false;
            });
        }
        if(list.empty()) {
            row(outputs.browser != nullptr && outputs.browser->searching() ? "  Searching..." : "  No network outputs", [] {});
        }

        rows[0]->layout(renderer);
        float row_width = rows[0]->width;
        float row_height = rows[0]->height;
        auto column = make_owned<VBox>(picker_widgets, row_width, row_height * static_cast<float>(rows.size()), rows, Justify::Top, 0.0f);
        // Bottom-justified: the spacer sits on the bottom edge, the column on top of it.
        float gap = 20.0f + button_height + 6.0f;  // pane inset + clock row + a little air
        auto spacer = make_owned<Spacer>(picker_widgets, 1.0f, gap);
        picker_root = make_owned<VBox>(picker_widgets, design_width, design_height, std::vector<Widget*>{spacer, column}, Justify::Bottom, 0.0f, Justify::Right, 20.0f);
    }

    void update(pyjuke::Player& player, const std::function<int64_t()>& now)
    {
        const pyjuke::TrackInfo& info = player.info();
        track_name->set_text(info.title);
        track_artist->set_text(info.artist);
        track_album->set_text(info.album);
        track_number->set_text(std::to_string(player.current_index()));
        mode_text->set_text(player.mode_name());
        position_text->set_text(pyjuke::format_time(player.position_ms()));
        duration_text->set_text(pyjuke::format_time(player.duration_ms()));
        int scroll = player.scroll_value();
        if(!time_scrollbar->is_grabbed() && scroll >= 0) {
            time_scrollbar->set_value(static_cast<float>(scroll));
        }

        std::string status;
        if(player.stopped_on_errors()) {
            status = "Stopped: could not play any track";
        } else if(!outputs.notice.empty()) {
            status = outputs.notice;
        } else if(outputs.remote && player.state() == pyjuke::PlayState::Playing) {
            status = "Playing on " + outputs.current_name();
        } else {
            status = player.state_name();
        }
        last_line->set_text(status);
        output_text->set_text(outputs.current_name());

        char clock[64];
        std::time_t t = std::time(nullptr);
        std::strftime(clock, sizeof(clock), "%I:%M %p, %a %b %d", std::localtime(&t));
        date_time_text->set_text(clock);

        // The picker layer follows picker_open and the renderer list.
        uint64_t generation = outputs.browser != nullptr ? outputs.browser->generation() : 0;
        bool searching = outputs.browser != nullptr && outputs.browser->searching();
        bool stale = picker_root == nullptr || generation != picker_generation || outputs.current_udn() != picker_current || searching != picker_searching;
        if(picker_open) {
            if(stale) {
                if(picker_shown) {
                    screen->pop();
                    picker_shown = false;
                }
                rebuild_picker(player, now);
                picker_generation = generation;
                picker_current = outputs.current_udn();
                picker_searching = searching;
            }
            if(!picker_shown) {
                screen->push(picker_root, 0.0f, 0.0f, true);
                picker_shown = true;
            }
        } else if(picker_shown) {
            screen->pop();
            picker_shown = false;
        }
    }
};

struct App
{
    gfx::Renderer* renderer = nullptr;
    pyjuke::Player* player = nullptr;
    Outputs* outputs = nullptr;
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
        case GLFW_KEY_O:
            if(app.outputs != nullptr) {
                app.outputs->cycle(player, now_ms());
            }
            if(app.screen != nullptr) {
                app.screen->picker_open = false;
            }
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
    bool handled = app.screen->screen->mouse_event(what, x, y);
    // The open picker is modal, so a press it does not take closes it.
    if(!handled && what == ui::Mouse::Press) {
        app.screen->picker_open = false;
    }
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
        pyjuke::AudioSwitch audio_switch(audio);

        // Network outputs: serve the library and look for renderers.
        std::unique_ptr<pyjuke::TrackServer> server;
        std::unique_ptr<pyjuke::upnp::RendererBrowser> browser;
        if(config.outputs) {
            server = std::make_unique<pyjuke::TrackServer>(tracks);  // its own copy; the Player takes the list
            std::string error;
            if(!server->start(static_cast<uint16_t>(config.stream_port), &error)) {
                fprintf(stderr, "network outputs disabled: %s\n", error.c_str());
                server.reset();
            } else {
                browser = std::make_unique<pyjuke::upnp::RendererBrowser>([] { glfwPostEmptyEvent(); });
            }
        }
        Outputs outputs(audio, audio_switch, server.get(), browser.get(), config.musicdir, config.default_output);

        pyjuke::PlayerTiming timing;
        timing.seek_wait_ms = config.seek_wait_millis;
        timing.seek_repeat_ms = config.seek_repeat_millis;
        timing.seek_delta_ms = config.seek_delta_millis;
        timing.reverse_skip_ms = config.reverse_skip_millis;

        pyjuke::Player player(audio_switch, std::move(tracks), config.musicdir, timing, std::random_device{}());
        JukeboxScreen screen(r, player, outputs, font, now_ms);
        app.player = &player;
        app.outputs = &outputs;
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
            if(outputs.tick(player, now)) {
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
                screen.update(player, now_ms);
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
        app.outputs = nullptr;
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
