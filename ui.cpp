#include "ui.h"

#include <algorithm>
#include <sstream>

namespace ui
{

// ---------------------------------------------------------------------------
// Container

Container::Container(float w, float h, const std::vector<Widget*>& children)
{
    width = w;
    height = h;
    for(Widget* child: children) {
        slots.push_back({child});
    }
}

void Container::layout(Renderer& r)
{
    for(auto& slot: slots) {
        slot.widget->layout(r);
    }
    place();
}

void Container::draw(Renderer& r, float x, float y)
{
    for(auto& slot: slots) {
        slot.widget->draw(r, x + slot.x, y + slot.y);
    }
}

bool Container::mouse_event(Mouse what, float x, float y)
{
    if(grabbing_child != nullptr) {
        // A child that took the press gets everything up to and including the release.
        for(auto& slot: slots) {
            if(slot.widget == grabbing_child) {
                bool handled = slot.widget->mouse_event(what, x - slot.x, y - slot.y);
                if(what == Mouse::Release) {
                    grabbing_child = nullptr;
                }
                return handled;
            }
        }
        grabbing_child = nullptr;
    }

    for(auto& slot: slots) {
        if(slot.widget->mouse_event(what, x - slot.x, y - slot.y)) {
            if(what == Mouse::Press) {
                grabbing_child = slot.widget;
            }
            return true;
        }
    }
    return false;
}

void Container::set_selected(bool selected)
{
    for(auto& slot: slots) {
        slot.widget->set_selected(selected);
    }
}

// ---------------------------------------------------------------------------
// HBox

HBox::HBox(float w, float h, const std::vector<Widget*>& children, Justify justify, float xpad, Justify vjustify, float vpad) :
    Container(w, h, children),
    justify(justify),
    xpad(xpad),
    vjustify(vjustify),
    vpad(vpad)
{
}

void HBox::place()
{
    auto yjust = [this](float child_height) {
        if(vjustify == Justify::Top) {
            return vpad;
        }
        if(vjustify == Justify::Bottom) {
            return height - vpad - child_height;
        }
        return (height - child_height) / 2;
    };

    if(justify == Justify::Left) {
        float xoffset = xpad;
        for(auto& slot: slots) {
            slot.x = xoffset;
            slot.y = yjust(slot.widget->height);
            xoffset += slot.widget->width + xpad * 2;
        }
    } else if(justify == Justify::Right) {
        float xoffset = width;
        for(auto& slot: slots) {
            xoffset -= slot.widget->width + xpad * 2;
            slot.x = xoffset;
            slot.y = yjust(slot.widget->height);
        }
    } else {
        float total_width = 0;
        for(auto& slot: slots) {
            total_width += slot.widget->width;
        }
        float spacing = (width - total_width) / (slots.size() + 1);
        float xoffset = spacing;
        for(auto& slot: slots) {
            slot.x = xoffset;
            slot.y = yjust(slot.widget->height);
            xoffset += slot.widget->width + spacing;
        }
    }
}

// ---------------------------------------------------------------------------
// VBox

VBox::VBox(float w, float h, const std::vector<Widget*>& children, Justify justify, float ypad, Justify hjustify, float hpad) :
    Container(w, h, children),
    justify(justify),
    ypad(ypad),
    hjustify(hjustify),
    hpad(hpad)
{
}

void VBox::place()
{
    auto hjust = [this](float child_width) {
        if(hjustify == Justify::Left) {
            return hpad;
        }
        if(hjustify == Justify::Right) {
            return width - hpad - child_width;
        }
        return (width - child_width) / 2;
    };

    if(justify == Justify::Top) {
        float yoffset = ypad;
        for(auto& slot: slots) {
            slot.x = hjust(slot.widget->width);
            slot.y = yoffset;
            yoffset += slot.widget->height + ypad * 2;
        }
    } else if(justify == Justify::Bottom) {
        float yoffset = height;
        for(auto& slot: slots) {
            yoffset -= slot.widget->height + ypad * 2;
            slot.x = hjust(slot.widget->width);
            slot.y = yoffset;
        }
    } else {
        float total_height = 0;
        for(auto& slot: slots) {
            total_height += slot.widget->height;
        }
        float spacing = (height - total_height) / (slots.size() + 1);
        float yoffset = spacing;
        for(auto& slot: slots) {
            slot.x = hjust(slot.widget->width);
            slot.y = yoffset;
            yoffset += slot.widget->height + spacing;
        }
    }
}

// ---------------------------------------------------------------------------
// Text

Text::Text(const std::string& text, float font_size, Color fgcolor, Color bgcolor, Justify justify) :
    font_size(font_size),
    fgcolor(fgcolor),
    bgcolor(bgcolor),
    justify(justify)
{
    set_text(text);
}

void Text::set_text(const std::string& text)
{
    lines.clear();
    std::istringstream stream(text);
    std::string line;
    while(std::getline(stream, line)) {
        lines.push_back(line);
    }
}

void Text::layout(Renderer& r)
{
    width = 0;
    for(const auto& line: lines) {
        width = std::max(width, r.text_width(line, font_size));
    }
    height = r.line_height(font_size) * lines.size();
}

void Text::draw(Renderer& r, float x, float y)
{
    // Selected text is drawn inverted; the widget behind it supplies the background.
    Color fg = selected ? bgcolor : fgcolor;
    float yoffset = 0;
    for(const auto& line: lines) {
        float xoffset = 0;
        if(justify == Justify::Right) {
            xoffset = width - r.text_width(line, font_size);
        } else if(justify == Justify::Center) {
            xoffset = (width - r.text_width(line, font_size)) / 2;
        }
        r.text(x + xoffset, y + yoffset, line, font_size, fg);
        yoffset += r.line_height(font_size);
    }
}

// ---------------------------------------------------------------------------
// BoxedText

BoxedText::BoxedText(float w, float h, const std::string& text, float font_size, Color fgcolor, Color bgcolor, Justify justify, float xpad) :
    HBox(w, h, {&textarea}, (justify == Justify::Center) ? Justify::Distribute : justify, xpad),
    textarea(text, font_size, fgcolor, bgcolor, justify)
{
}

void BoxedText::draw(Renderer& r, float x, float y)
{
    // Clip horizontally only; a line of text may be taller than its box.
    r.push_clip({x, y - textarea.height, width, height + textarea.height * 2});
    HBox::draw(r, x, y);
    r.pop_clip();
}

// ---------------------------------------------------------------------------
// Bordered

Bordered::Bordered(Widget* child, Color color, float border_width) :
    Container(0, 0, {child}),
    color(color),
    border_width(border_width)
{
    place();
}

void Bordered::place()
{
    slots[0].x = border_width;
    slots[0].y = border_width;
    width = slots[0].widget->width + border_width * 2;
    height = slots[0].widget->height + border_width * 2;
}

void Bordered::draw(Renderer& r, float x, float y)
{
    r.rounded_rect({x, y, width, height}, 0, black, border_width, color);
    Container::draw(r, x, y);
}

// ---------------------------------------------------------------------------
// RoundedButton

RoundedButton::RoundedButton(float line_width, float corner_radius, Color line_color, Color button_color, Widget* child, float child_pad) :
    line_width(line_width),
    corner_radius(corner_radius),
    line_color(line_color),
    button_color(button_color),
    child(child),
    child_pad(child_pad)
{
}

void RoundedButton::layout(Renderer& r)
{
    child->layout(r);
    width = child->width + line_width * 2 + child_pad * 2;
    height = child->height + line_width * 2 + child_pad * 2;
}

void RoundedButton::draw(Renderer& r, float x, float y)
{
    // Pressed with the pointer inside: filled with the border colour.
    Color fill = (selected && grabbed) ? line_color : button_color;
    r.rounded_rect({x, y, width, height}, corner_radius, fill, line_width, line_color);
    child->draw(r, x + line_width + child_pad, y + line_width + child_pad);
}

bool RoundedButton::hit(float x, float y) const
{
    return x >= 0 && x <= width && y >= 0 && y <= height;
}

bool RoundedButton::mouse_event(Mouse what, float x, float y)
{
    if(!grabbed && !hit(x, y)) {
        return false;
    }

    if(what == Mouse::Press) {
        grabbed = true;
        selected = true;
        child->set_selected(true);
        on_press();
    } else if(what == Mouse::Motion) {
        if(grabbed) {
            selected = hit(x, y);
            child->set_selected(selected);
        }
    } else {
        if(grabbed) {
            // Clear state first; the callback may change what the button shows.
            bool inside = hit(x, y);
            grabbed = false;
            selected = false;
            child->set_selected(false);
            if(inside) {
                on_release_inside();
            } else {
                on_release_outside();
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Scrollbar

Scrollbar::Scrollbar(float w, float h, float min, float max, const Style& style) :
    style(style),
    min(min),
    max(max),
    value(min)
{
    width = w;
    height = h;
}

void Scrollbar::set_value(float v)
{
    value = std::clamp(v, min, max);
}

float Scrollbar::thumb_left() const
{
    float normalized = (max > min) ? (value - min) / (max - min) : 0;
    return thumb_inset() + normalized * thumb_travel();
}

float Scrollbar::pixel_to_value(float pixel) const
{
    float travel = thumb_travel();
    if(travel <= 0) {
        return min;
    }
    return min + (max - min) * std::clamp(pixel - thumb_inset(), 0.0f, travel) / travel;
}

void Scrollbar::draw(Renderer& r, float x, float y)
{
    r.rounded_rect({x, y, width, height}, style.corner_radius, style.inner_color, style.border_width, style.border_color);
    float thumb_radius = std::max(0.0f, style.corner_radius - style.border_width - style.thumb_spacing);
    float thick = thickness() - thumb_inset() * 2;
    if(style.vertical) {
        r.rounded_rect({x + thumb_inset(), y + height - thumb_left() - thumb_width(), thick, thumb_width()}, thumb_radius, style.thumb_color);
    } else {
        r.rounded_rect({x + thumb_left(), y + thumb_inset(), thumb_width(), thick}, thumb_radius, style.thumb_color);
    }
}

bool Scrollbar::mouse_event(Mouse what, float x, float y)
{
    float a = along(x, y);
    if(!grabbed) {
        float c = across(x, y);
        bool on_thumb = a >= thumb_left() && a <= thumb_left() + thumb_width() && c >= thumb_inset() && c <= thickness() - thumb_inset();
        if(!on_thumb) {
            return false;
        }
    }

    if(what == Mouse::Press) {
        grabbed = true;
        drag_offset = a - thumb_left();
        on_scroll_start(value);
    } else if(grabbed) {
        set_value(pixel_to_value(a - drag_offset));
        if(what == Mouse::Motion) {
            on_scroll_continue(value);
        } else {
            grabbed = false;
            on_scroll_finish(value);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Screen

void Screen::push(Widget* child, float x, float y, bool modal)
{
    layers.push_back({child, x, y, modal});
}

void Screen::pop()
{
    if(layers.empty()) {
        return;
    }
    if(layers.back().child == grabbing_layer) {
        grabbing_layer = nullptr;
    }
    layers.pop_back();
}

void Screen::draw(Renderer& r)
{
    for(auto& layer: layers) {
        layer.child->layout(r);
        layer.child->draw(r, layer.x, layer.y);
    }
}

bool Screen::dispatch(Layer& layer, Mouse what, float x, float y)
{
    return layer.child->mouse_event(what, x - layer.x, y - layer.y);
}

bool Screen::mouse_event(Mouse what, float x, float y)
{
    if(grabbing_layer != nullptr) {
        // The layer that took the press gets everything up to and including the release.
        for(auto& layer: layers) {
            if(layer.child == grabbing_layer) {
                bool handled = dispatch(layer, what, x, y);
                if(what == Mouse::Release) {
                    grabbing_layer = nullptr;
                }
                return handled;
            }
        }
        grabbing_layer = nullptr;
    }

    // Copy each layer before dispatching: a callback may push or pop layers.
    for(size_t i = layers.size(); i-- > 0;) {
        Layer layer = layers[i];
        if(dispatch(layer, what, x, y)) {
            if(what == Mouse::Press) {
                grabbing_layer = layer.child;
            }
            return true;
        }
        if(layer.modal) {
            return false;
        }
    }
    return false;
}

};
