#include "../../include/ui/cleanup_view.hpp"

#include "../../include/utils/format.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>

using namespace ftxui;

namespace {

// TAB cycles through them
const char* FILTERS[] = {"all", "caches and builds", "old data", "files"};
constexpr int FILTER_COUNT = 4;

bool keeps(int filter, const Scanner::CleanupItem& item) {
    switch (filter) {
        case 1: return !item.is_file && item.kind != "old data";
        case 2: return item.kind == "old data";
        case 3: return item.is_file;
        default: return true;
    }
}

Color kindColor(const std::string& kind) {
    if (kind == "old data")   return Color::RGB(255, 140, 0);
    if (kind == "core dump")  return Color::RGB(255, 70, 70);
    if (kind == "large file") return Color::RGB(30, 144, 255);
    return Color::RGB(60, 179, 113);
}

// Room left for the path: the window is at most 120 columns, minus borders and the other columns
size_t pathWidth() {
    const int width = std::min(120, ftxui::Terminal::Size().dimx) - 2 - 38 - 1;
    return static_cast<size_t>(std::max(width, 12));
}

}

void BonsaiCleanup::fill(State& state, Scanner& scanner) {
    state.all = scanner.cleanup();
    state.partial = !scanner.isDone();
    state.now = std::time(nullptr);
    apply(state);
}

void BonsaiCleanup::apply(State& state) {
    state.items.clear();
    state.labels.clear();

    for (const auto& item : state.all) {
        if (!keeps(state.filter, item)) continue;
        state.items.push_back(item);
        state.labels.push_back(item.path);
    }

    state.selected = 0;
}

std::string BonsaiCleanup::toText(const std::vector<Scanner::CleanupItem>& items, const UserScope& scope, size_t max_items) {
    std::ostringstream out;
    const int64_t now = std::time(nullptr);
    uint64_t total = 0;

    for (const auto& item : items)
        total += item.disk;

    out << "Cleanup suggestions: " << items.size() << " entries, " << FormatUtils::toReadable(total, " ") << " on disk\n\n";
    out << "  Size   Files  Changed  Kind            Path\n";

    for (size_t i = 0; i < items.size() && i < max_items; i++) {
        const auto& item = items[i];
        char line[96];
        std::snprintf(line, sizeof(line), "%6s %7s %8s  %-15s ", FormatUtils::toCompact(item.disk).c_str(),
                      item.is_file ? "" : FormatUtils::toCount(item.inodes).c_str(),
                      item.newest > 0 ? FormatUtils::toAge(now - item.newest).c_str() : "", item.kind.c_str());
        out << line << scope.display(item.path) << "\n";
    }

    if (items.size() > max_items)
        out << "  ... and " << items.size() - max_items << " more (bonsai-romeo, key C)\n";

    return out.str();
}

Component BonsaiCleanup::window(std::shared_ptr<State> state, const UserScope* scope,
                                std::function<void(const Scanner::CleanupItem&)> on_open,
                                std::function<void()> on_close) {
    MenuOption option;

    // Columns: size on disk, files, last change, kind, path
    option.entries_option.transform = [state, scope](const EntryState& entry) {
        if (entry.index < 0 || entry.index >= static_cast<int>(state->items.size()))
            return text("");

        const auto& item = state->items[entry.index];
        const std::string age = item.newest > 0 ? FormatUtils::toAge(state->now - item.newest) : "";

        auto row = hbox({
            text(FormatUtils::toCompact(item.disk)) | size(WIDTH, EQUAL, 7),
            text(item.is_file ? "" : FormatUtils::toCount(item.inodes)) | size(WIDTH, EQUAL, 7),
            text(age) | size(WIDTH, EQUAL, 8),
            text(item.kind) | color(kindColor(item.kind)) | size(WIDTH, EQUAL, 16),
            text(FormatUtils::trimMiddle(scope->display(item.path), pathWidth())) | flex,
        });

        return entry.active ? row | bgcolor(Color::White) | color(Color::Black) : row;
    };

    option.on_enter = [state, on_open] {
        if (state->selected >= 0 && state->selected < static_cast<int>(state->items.size()))
            on_open(state->items[state->selected]);
    };

    auto menu = Menu(&state->labels, &state->selected, option);

    auto view = Renderer(menu, [state, menu] {
        uint64_t total = 0;
        for (const auto& item : state->items)
            total += item.disk;

        Element list = state->items.empty()
            ? text(" Nothing to suggest: no cache, build folder, old data or large file found.") | color(Color::GrayLight)
            : menu->Render() | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 20);

        std::string hint;
        if (state->selected >= 0 && state->selected < static_cast<int>(state->items.size()))
            hint = state->items[state->selected].hint;

        return vbox({
            hbox({
                text(" Cleanup suggestions ") | bold,
                text("[" + std::string(FILTERS[state->filter]) + "]") | color(Color::GrayLight),
                filler(),
                text(std::to_string(state->items.size()) + " entries, " + FormatUtils::toReadable(total, " ") + " on disk ") | color(Color::GrayLight),
            }),
            state->partial ? text(" The scan is still running: more may show up, sizes may grow") | color(Color::RGB(255, 215, 0)) : emptyElement(),
            separator(),
            hbox({
                text("Size") | size(WIDTH, EQUAL, 7),
                text("Files") | size(WIDTH, EQUAL, 7),
                text("Changed") | size(WIDTH, EQUAL, 8),
                text("Kind") | size(WIDTH, EQUAL, 16),
                text("Path"),
            }) | color(Color::GrayDark),
            list,
            separator(),
            paragraph(" " + hint),
            text(" ENTER open in the explorer (then D to delete)   TAB kind   ESC close") | color(Color::GrayDark),
        }) | border | size(WIDTH, LESS_THAN, 120);
    });

    return CatchEvent(view, [state, on_close](Event event) {
        if (event == Event::Escape || event == Event::Character('c') || event == Event::Character('C')) {
            on_close();
            return true;
        }

        if (event == Event::Tab) {
            state->filter = (state->filter + 1) % FILTER_COUNT;
            apply(*state);
            return true;
        }

        return false;
    });
}
