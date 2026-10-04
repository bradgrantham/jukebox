// Widget toolkit; a port of uglypygui.py onto the Vulkan Renderer.
//
// All coordinates are in the Renderer's design space.  Widgets do not own
// their children; the application keeps every widget alive for as long as the
// tree is in use.  The whole tree is laid out and redrawn each frame, so there
// is no dirty tracking.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "renderer.h"

namespace ui
{

using gfx::Color;
using gfx::Rect;
using gfx::Renderer;

enum class Justify { Left, Distribute, Center, Right, Top, Bottom };
enum class Mouse { Press, Motion, Release };

inline constexpr Color white{1.0f, 1.0f, 1.0f, 1.0f};
inline constexpr Color black{0.0f, 0.0f, 0.0f, 1.0f};

class Widget
{
public:
    virtual ~Widget() = default;

    float width = 0;
    float height = 0;

    // Recompute sizes and child positions; called on the whole tree before each draw.
    virtual void layout(Renderer&) {}
    virtual void draw(Renderer& r, float x, float y) = 0;
    // Point is relative to the widget's top left.  Returns true if handled.
    virtual bool mouse_event(Mouse, float, float) { return false; }
    virtual void set_selected(bool) {}
};

class Spacer : public Widget
{
public:
    Spacer(float w, float h) { width = w; height = h; }
    void draw(Renderer&, float, float) override {}
};

class Container : public Widget
{
public:
    void layout(Renderer& r) override;
    void draw(Renderer& r, float x, float y) override;
    bool mouse_event(Mouse what, float x, float y) override;
    void set_selected(bool selected) override;

protected:
    struct Slot { Widget* widget; float x = 0; float y = 0; };

    Container(float w, float h, const std::vector<Widget*>& children);
    virtual void place() = 0;

    std::vector<Slot> slots;

private:
    Widget* grabbing_child = nullptr;
};

class HBox : public Container
{
public:
    HBox(float w, float h, const std::vector<Widget*>& children, Justify justify = Justify::Distribute, float xpad = 1, Justify vjustify = Justify::Center, float vpad = 1);

protected:
    void place() override;

private:
    Justify justify;
    float xpad;
    Justify vjustify;
    float vpad;
};

class VBox : public Container
{
public:
    VBox(float w, float h, const std::vector<Widget*>& children, Justify justify = Justify::Distribute, float ypad = 1, Justify hjustify = Justify::Center, float hpad = 1);

protected:
    void place() override;

private:
    Justify justify;
    float ypad;
    Justify hjustify;
    float hpad;
};

class Text : public Widget
{
public:
    Text(const std::string& text, float font_size, Color fgcolor = white, Color bgcolor = black, Justify justify = Justify::Center);

    void set_text(const std::string& text);
    void layout(Renderer& r) override;
    void draw(Renderer& r, float x, float y) override;
    void set_selected(bool sel) override { selected = sel; }

private:
    std::vector<std::string> lines;
    float font_size;
    Color fgcolor;
    Color bgcolor;
    Justify justify;
    bool selected = false;
};

// Text in a fixed-size box; text wider than the box is clipped.
class BoxedText : public HBox
{
public:
    BoxedText(float w, float h, const std::string& text, float font_size, Color fgcolor = white, Color bgcolor = black, Justify justify = Justify::Center, float xpad = 3);

    void set_text(const std::string& text) { textarea.set_text(text); }
    void draw(Renderer& r, float x, float y) override;

private:
    Text textarea;
};

class Bordered : public Container
{
public:
    explicit Bordered(Widget* child, Color color = white, float border_width = 3);

    void draw(Renderer& r, float x, float y) override;

protected:
    void place() override;

private:
    Color color;
    float border_width;
};

class RoundedButton : public Widget
{
public:
    // Sized to fit the child plus border and padding.
    RoundedButton(float line_width, float corner_radius, Color line_color, Color button_color, Widget* child, float child_pad = 10);

    std::function<void()> on_press = [] {};
    std::function<void()> on_release_inside = [] {};
    std::function<void()> on_release_outside = [] {};

    void layout(Renderer& r) override;
    void draw(Renderer& r, float x, float y) override;
    bool mouse_event(Mouse what, float x, float y) override;

private:
    bool hit(float x, float y) const;

    float line_width;
    float corner_radius;
    Color line_color;
    Color button_color;
    Widget* child;
    float child_pad;
    bool grabbed = false;
    bool selected = false;
};

class Scrollbar : public Widget
{
public:
    struct Style
    {
        float border_width = 2;
        Color border_color = white;
        float corner_radius = 7;
        float thumb_spacing = 2;
        Color inner_color = black;
        float thumb_fraction = .05f;
        Color thumb_color = white;
    };

    Scrollbar(float w, float h, float min, float max, const Style& style);

    std::function<void(float)> on_scroll_start = [](float) {};
    std::function<void(float)> on_scroll_continue = [](float) {};
    std::function<void(float)> on_scroll_finish = [](float) {};

    void set_value(float value);
    float get_value() const { return value; }
    bool is_grabbed() const { return grabbed; }

    void draw(Renderer& r, float x, float y) override;
    bool mouse_event(Mouse what, float x, float y) override;

private:
    float thumb_inset() const { return style.border_width + style.thumb_spacing; }
    float thumb_width() const { return (width - thumb_inset() * 2) * style.thumb_fraction; }
    float thumb_travel() const { return width - thumb_inset() * 2 - thumb_width(); }
    float thumb_left() const;
    float pixel_to_value(float pixel) const;

    Style style;
    float min;
    float max;
    float value;
    bool grabbed = false;
    float drag_offset = 0;
};

// Top of the tree: one child at a fixed offset in design space.
class Screen
{
public:
    Screen(Widget* child, float x, float y) : child(child), child_x(x), child_y(y) {}

    void draw(Renderer& r);
    bool mouse_event(Mouse what, float x, float y);

private:
    Widget* child;
    float child_x;
    float child_y;
};

};
