#include "../../include/ui/growth_view.hpp"

#include "../../include/config/config.hpp"
#include "../../include/utils/format.hpp"

#include "../../include/ui/focus.hpp"
#include "../../include/ui/theme.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <ctime>

using namespace ftxui;

namespace {

size_t pathWidth() {
    const int width = std::min(120, ftxui::Terminal::Size().dimx) - 2 - 20 - 1;
    return static_cast<size_t>(std::max(width, 12));
}

std::string amount(int64_t value, bool files) {
    const uint64_t v = static_cast<uint64_t>(std::max<int64_t>(value, 0));
    return files ? FormatUtils::toCount(v) : FormatUtils::toCompact(v);
}

}

void BonsaiGrowth::set(State& state, std::vector<Scanner::GrowthItem> items, bool partial, bool files, int64_t since) {
    state.items = std::move(items);
    state.labels.clear();
    for (const auto& item : state.items)
        state.labels.push_back(item.path);

    state.selected = 0;
    state.computing = false;
    state.partial = partial;
    state.files = files;
    state.since = since;
    state.now = std::time(nullptr);
}

Component BonsaiGrowth::window(std::shared_ptr<State> state, Scanner* scanner,
                               std::function<void(const std::string&)> on_open,
                               std::function<void()> on_close) {
    MenuOption option;

    // Columns: growth, now, new, path
    option.entries_option.transform = [state, scanner](const EntryState& entry) {
        if (entry.index < 0 || entry.index >= static_cast<int>(state->items.size()))
            return text("");

        const auto& item = state->items[entry.index];
        const int64_t now = static_cast<int64_t>(state->files ? item.now.inodes : scanner->value(item.now));

        auto row = hbox({
            text("+" + amount(item.delta, state->files)) | color(ui::theme::yellow()) | size(WIDTH, EQUAL, 8),
            text(amount(now, state->files)) | size(WIDTH, EQUAL, 7),
            text(item.is_new ? "new" : "") | color(ui::theme::green()) | size(WIDTH, EQUAL, 5),
            text(FormatUtils::trimMiddle(scanner->getScope().display(item.path), pathWidth())) | flex,
        });

        return entry.active ? row | inverted : row;
    };

    option.on_enter = [state, on_open] {
        if (state->selected >= 0 && state->selected < static_cast<int>(state->items.size()))
            on_open(state->items[state->selected].path);
    };

    auto menu = Menu(&state->labels, &state->selected, option);

    auto view = Renderer(menu, [state, menu] {
        const std::string unit = state->files ? "files" : "size";
        const std::string least = state->files ? "1 000 files" : "100 MiB";

        Element list;
        if (state->computing)
            list = text(" Comparing with the previous scan...") | dim;
        else if (state->since == 0)
            list = paragraph(" No previous scan to compare with: run bonsai-romeo again later, this will list what grew in between.") | dim;
        else if (state->items.empty())
            list = paragraph(" No folder grew by " + least + " or more since the previous scan.") | dim;
        else
            list = menu->Render() | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 20);

        const std::string since = state->since > 0 ? FormatUtils::toAge(state->now - state->since) + " ago" : "";

        return ui::overlay(ui::panel("Grown since the previous scan", vbox({
            state->partial ? text(" The scan is still running: folders show up once scanned") | color(ui::theme::yellow()) : emptyElement(),
            hbox({
                text("GROWN") | size(WIDTH, EQUAL, 8),
                text("NOW") | size(WIDTH, EQUAL, 7),
                text("") | size(WIDTH, EQUAL, 5),
                text("PATH"),
            }) | dim,
            list,
            separator() | dim,
            paragraph(" Net growth, where it happened: a folder that grew mostly in one sub-folder shows that sub-folder.") | dim,
            hbox({text(" "), ui::keyHints({{"enter", "open"}, {"f", "files / size"}, {"esc", "close"}})}),
        }), ui::theme::accent(), text(unit + (since.empty() ? "" : " · previous scan " + since) + " · " + std::to_string(state->items.size()) + " folders") | dim)
            | size(WIDTH, LESS_THAN, 122));
    });

    return keepFocus(CatchEvent(view, [on_close](Event event) {
        if (event == Event::Escape || event == Event::Character('w') || event == Event::Character('W')) {
            on_close();
            return true;
        }
        return false;
    }));
}
