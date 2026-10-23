#include "../../include/ui/help_view.hpp"

#include "../../include/ui/focus.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

namespace {

struct Key {
    const char* keys;
    const char* what;
};

struct Group {
    const char* title;
    std::vector<Key> keys;
};

const std::vector<Group> GROUPS = {
    {"Moving", {
        {"ENTER  l", "open the folder"},
        {"BKSP  ESC  h", "go up (from a space: back to your spaces)"},
        {"UP DOWN  j k", "select"},
        {"g G  PGUP PGDN", "first, last, one page up or down"},
        {"LEFT RIGHT", "narrower or wider sidebar (saved)"},
    }},
    {"Showing", {
        {"F", "size, size on disk, number of files (and the order)"},
        {"S", "order: size, name, oldest change first"},
        {"A", "column: time since the last change"},
        {"R", "scan the folder shown again (in the quota view: read the quotas again)"},
    }},
    {"Cleaning", {
        {"SPACE  *", "mark or unmark the entry; * every entry of the folder (X clears)"},
        {"D  SUPPR", "delete the entry, or every marked one (asks first)"},
        {"T", "pack a folder into one .tar: thousands of files become one (asks first)"},
        {"Y", "copy the path (terminals with OSC 52: iTerm2, Ghostty, WezTerm, kitty...)"},
    }},
    {"Windows", {
        {"/", "search folders by name"},
        {"C", "cleanup suggestions"},
        {"U", "quotas; 1 to 9 there opens that space in the explorer"},
        {"W", "what grew since the previous scan"},
        {"P", "progress of a deletion, packing or scan running (S there stops it)"},
        {"!", "folders that couldn't be read (when there are)"},
        {"?", "this help"},
        {"Q", "quit"},
    }},
};

}

Component BonsaiHelp::window(std::function<void()> on_close) {
    auto view = Renderer([] {
        Elements rows;
        for (const auto& group : GROUPS) {
            rows.push_back(text(std::string(" ") + group.title) | bold);
            for (const auto& key : group.keys) {
                rows.push_back(hbox({
                    text("   " + std::string(key.keys)) | color(Color::RGB(255, 215, 0)) | size(WIDTH, EQUAL, 20),
                    text(key.what),
                }));
            }
            rows.push_back(text(""));
        }

        return vbox({
            text(" Keys ") | bold | center,
            separator(),
            vbox(std::move(rows)) | vscroll_indicator | yframe | size(HEIGHT, LESS_THAN, 34),
            separator(),
            text(" Sizes: ~ from the previous scan (still scanning)   ? not scanned   ! unreadable folder in it") | color(Color::GrayDark),
            text(" ESC close") | color(Color::GrayDark),
        }) | border | size(WIDTH, LESS_THAN, 100);
    });

    return keepFocus(CatchEvent(view, [on_close](Event event) {
        if (event == Event::Escape || event == Event::Character('?') || event == Event::Return) {
            on_close();
            return true;
        }
        return false;
    }));
}
