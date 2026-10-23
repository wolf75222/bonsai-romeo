#include "../../include/ui/cleanup_view.hpp"

#include "../../include/utils/format.hpp"
#include "../../include/utils/json.hpp"

#include "../../include/ui/focus.hpp"

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
const char* FILTERS[] = {"all", "caches and builds", "old data", "many files", "files"};
constexpr int FILTER_COUNT = 5;

bool keeps(int filter, const Scanner::CleanupItem& item) {
    switch (filter) {
        case 1: return !item.is_file && item.kind != "old data" && item.kind != "many files";
        case 2: return item.kind == "old data" || item.kind == "old file";
        case 3: return item.kind == "many files";
        case 4: return item.is_file;
        default: return true;
    }
}

Color kindColor(const std::string& kind) {
    if (kind == "old data" || kind == "old file") return Color::RGB(255, 140, 0);
    if (kind == "many files") return Color::RGB(186, 85, 211);
    if (kind == "core dump")  return Color::RGB(255, 70, 70);
    if (kind == "large file") return Color::RGB(30, 144, 255);
    return Color::RGB(60, 179, 113);
}

// The thresholds O goes through
const int AGES[] = {90, 180, 365, 730};

std::string ageName(int days) {
    switch (days) {
        case 90:  return "3 months";
        case 180: return "6 months";
        case 365: return "1 year";
        case 730: return "2 years";
        default:  return std::to_string(days) + " days";
    }
}

// "12 entries, 3.40 GiB on disk, 120k files"
std::string summary(const std::vector<Scanner::CleanupItem>& items) {
    uint64_t disk = 0, files = 0;
    for (const auto& item : items) {
        disk += item.disk;
        files += item.inodes;
    }
    return std::to_string(items.size()) + " entries, " + FormatUtils::toReadable(disk, " ") + " on disk, " + FormatUtils::toCount(files) + " files";
}

// Room left for the path: the window is at most 120 columns, minus borders and the other columns
size_t pathWidth() {
    const int width = std::min(120, ftxui::Terminal::Size().dimx) - 2 - 38 - 1;
    return static_cast<size_t>(std::max(width, 12));
}

}

void BonsaiCleanup::set(State& state, std::vector<Scanner::CleanupItem> items, bool partial) {
    state.all = std::move(items);
    state.partial = partial;
    state.computing = false;
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

    out << "Cleanup suggestions: " << summary(items) << "\n\n";
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

std::string BonsaiCleanup::toJson(const std::vector<Scanner::CleanupItem>& items, const UserScope& scope) {
    std::ostringstream out;
    out << "{\"items\": [";
    for (size_t i = 0; i < items.size(); i++) {
        const auto& item = items[i];
        out << (i ? ",\n  " : "\n  ") << "{\"path\": " << Json::str(item.path) << ", \"shown\": " << Json::str(scope.display(item.path))
            << ", \"kind\": " << Json::str(item.kind) << ", \"hint\": " << Json::str(item.hint)
            << ", \"disk\": " << item.disk << ", \"files\": " << item.inodes << ", \"newest\": " << item.newest
            << ", \"is_file\": " << (item.is_file ? "true" : "false") << "}";
    }
    out << "\n]}\n";
    return out.str();
}

Component BonsaiCleanup::window(std::shared_ptr<State> state, const UserScope* scope,
                                std::function<void(const Scanner::CleanupItem&)> on_open,
                                std::function<void()> on_close,
                                std::function<void()> on_age) {
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
        Element list = state->computing
            ? text(" Looking for what could be cleaned...") | color(Color::GrayLight)
            : state->items.empty()
            ? text(" Nothing to suggest: no cache, build folder, old data, folder of many files or large file found.") | color(Color::GrayLight)
            : menu->Render() | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 20);

        std::string hint;
        if (state->selected >= 0 && state->selected < static_cast<int>(state->items.size()))
            hint = state->items[state->selected].hint;

        return vbox({
            hbox({
                text(" Cleanup suggestions ") | bold,
                text("[" + std::string(FILTERS[state->filter]) + "]  ") | color(Color::GrayLight),
                text("old: " + ageName(state->old_days) + "  ") | color(Color::GrayDark),
                filler(),
                text(summary(state->items) + " ") | color(Color::GrayLight),
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
            text(" ENTER open (then D delete, T pack)   TAB kind   O age   ESC close") | color(Color::GrayDark),
        }) | border | size(WIDTH, LESS_THAN, 120);
    });

    return keepFocus(CatchEvent(view, [state, on_close, on_age](Event event) {
        if (event == Event::Escape || event == Event::Character('c') || event == Event::Character('C')) {
            on_close();
            return true;
        }

        if (event == Event::Tab) {
            state->filter = (state->filter + 1) % FILTER_COUNT;
            apply(*state);
            return true;
        }

        // What counts as old: the next threshold, looked for again
        if (event == Event::Character('o') || event == Event::Character('O')) {
            const auto* at = std::find(std::begin(AGES), std::end(AGES), state->old_days);
            state->old_days = at == std::end(AGES) || at + 1 == std::end(AGES) ? AGES[0] : *(at + 1);
            on_age();
            return true;
        }

        return false;
    }));
}
