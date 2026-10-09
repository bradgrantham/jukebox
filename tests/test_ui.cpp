// tests/test_ui.cpp - mouse routing through Screen layers and containers.
// Widgets are sized by hand instead of laid out, so no Renderer is involved.

#undef NDEBUG
#include <cassert>

#include "ui.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

void section(const char* name) { std::printf("  %s\n", name); }

// A fixed-size widget that counts the presses it takes.
struct Pad : ui::Widget
{
    int presses = 0, releases = 0, motions = 0;
    bool take = true;  // whether it claims events inside its box
    Pad(float w, float h) { width = w; height = h; }
    void draw(ui::Renderer&, float, float) override {}
    bool mouse_event(ui::Mouse what, float x, float y) override
    {
        if(!take || x < 0 || y < 0 || x > width || y > height) {
            return false;
        }
        if(what == ui::Mouse::Press) ++presses;
        else if(what == ui::Mouse::Release) ++releases;
        else ++motions;
        return true;
    }
};

void test_layers()
{
    section("layers: front gets events first, back is drawn behind");
    Pad back(800, 480), front(200, 100);
    ui::Screen screen(&back, 0, 0);
    assert(screen.depth() == 1);
    assert(screen.mouse_event(ui::Mouse::Press, 10, 10));
    assert(screen.mouse_event(ui::Mouse::Release, 10, 10));
    assert(back.presses == 1 && back.releases == 1);

    screen.push(&front, 300, 200);
    assert(screen.depth() == 2);
    // Inside the front layer: front takes it, back never sees it.
    assert(screen.mouse_event(ui::Mouse::Press, 310, 210));
    assert(screen.mouse_event(ui::Mouse::Release, 310, 210));
    assert(front.presses == 1 && front.releases == 1);
    assert(back.presses == 1);
    // Outside the (non-modal) front layer: falls through to the back.
    assert(screen.mouse_event(ui::Mouse::Press, 10, 10));
    assert(screen.mouse_event(ui::Mouse::Release, 10, 10));
    assert(back.presses == 2 && front.presses == 1);

    section("layers: a press grabs its layer until release");
    assert(screen.mouse_event(ui::Mouse::Press, 310, 210));
    // Drag out of the front layer: still delivered to it, not the back.
    assert(screen.mouse_event(ui::Mouse::Motion, 10, 10) == false);  // front declines (outside its box)
    assert(back.motions == 0);
    assert(screen.mouse_event(ui::Mouse::Release, 10, 10) == false);
    assert(back.releases == 2 && front.releases == 1);
    // After the release the grab is gone.
    assert(screen.mouse_event(ui::Mouse::Press, 10, 10));
    assert(back.presses == 3);
    assert(screen.mouse_event(ui::Mouse::Release, 10, 10));

    section("layers: pop clears a grab held by that layer");
    assert(screen.mouse_event(ui::Mouse::Press, 310, 210));
    screen.pop();
    assert(screen.depth() == 1);
    assert(screen.mouse_event(ui::Mouse::Release, 310, 210));  // goes to the back now
    assert(back.releases == 4 && front.releases == 1);
}

void test_modal()
{
    section("modal layer swallows what it does not handle");
    Pad back(800, 480), popup(200, 100);
    ui::Screen screen(&back, 0, 0);
    screen.push(&popup, 300, 200, true);
    assert(!screen.mouse_event(ui::Mouse::Press, 10, 10));   // unhandled: reported, not passed on
    assert(back.presses == 0);
    assert(screen.mouse_event(ui::Mouse::Press, 320, 220));
    assert(popup.presses == 1);
    assert(screen.mouse_event(ui::Mouse::Release, 320, 220));
    screen.pop();
    assert(screen.mouse_event(ui::Mouse::Press, 10, 10));
    assert(back.presses == 1);
}

void test_callbacks_may_change_layers()
{
    section("a callback may pop the layer it lives in");
    Pad back(800, 480);
    ui::Screen screen(&back, 0, 0);
    ui::Text label("x", 10);
    ui::RoundedButton button(2, 4, ui::white, ui::black, &label);
    button.width = 100;
    button.height = 40;
    int fired = 0;
    button.on_release_inside = [&] {
        ++fired;
        screen.pop();
    };
    screen.push(&button, 100, 100, true);
    assert(screen.mouse_event(ui::Mouse::Press, 110, 110));
    assert(screen.mouse_event(ui::Mouse::Release, 110, 110));
    assert(fired == 1);
    assert(screen.depth() == 1);
    assert(screen.mouse_event(ui::Mouse::Press, 110, 110));
    assert(back.presses == 1);
}

void test_container_order()
{
    section("container: first child to claim a press grabs it");
    Pad a(100, 100), b(100, 100);
    ui::HBox row(220, 100, {&a, &b}, ui::Justify::Left, 5);
    // Place by hand: HBox::place() is what layout() would call.
    struct Exposed : ui::HBox { using HBox::place; };
    static_cast<Exposed&>(row).place();
    assert(row.mouse_event(ui::Mouse::Press, 150, 50));
    assert(b.presses == 1 && a.presses == 0);
    assert(!row.mouse_event(ui::Mouse::Release, 50, 50));  // b declines outside; a is not asked
    assert(a.releases == 0);
}

} // namespace

int main()
{
    std::printf("ui_tests\n");
    test_layers();
    test_modal();
    test_callbacks_may_change_layers();
    test_container_order();
    std::printf("all tests passed\n");
    return 0;
}
