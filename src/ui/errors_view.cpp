#include "../../include/ui/errors_view.hpp"

#include "../../include/utils/format.hpp"

#include "../../include/ui/focus.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>

using namespace ftxui;

namespace {

std::string message(int error) {
    return error > 0 ? std::strerror(error) : "unknown error";
}

// What to do about it
std::string advice(int error) {
    if (error == EACCES || error == EPERM)
        return "A folder of yours you can't read: chmod u+rx on it, then R in the folder above.";
    if (error == ENOMEM || error == EMFILE || error == ENFILE)
        return "Out of memory or file handles: run bonsai-romeo again (with BONSAI_THREADS=8 if it comes back).";
    return "GPFS couldn't read it (I/O error, stale handle, timeout): press R in the folder above later. "
           "If it stays, tell ROMEO's support (romeo-ticket.univ-reims.fr) which folder.";
}

size_t pathWidth() {
    const int width = std::min(110, ftxui::Terminal::Size().dimx) - 2 - 26 - 1;
    return static_cast<size_t>(std::max(width, 12));
}

}

void BonsaiErrors::set(State& state, std::vector<Scanner::ReadError> errors, uint32_t total) {
    state.errors = std::move(errors);
    state.total = std::max<uint32_t>(total, static_cast<uint32_t>(state.errors.size()));
    state.labels.clear();
    for (const auto& e : state.errors)
        state.labels.push_back(e.path);
    state.selected = 0;
}

std::string BonsaiErrors::toText(const std::vector<Scanner::ReadError>& errors, uint32_t total, const UserScope& scope) {
    if (total == 0)
        return "";

    std::ostringstream out;
    out << "! " << total << " folder" << (total > 1 ? "s" : "") << " couldn't be read completely: the sizes above them may be too low\n";
    for (size_t i = 0; i < errors.size() && i < 10; i++)
        out << "    " << scope.display(errors[i].path) << ": " << message(errors[i].error) << "\n";
    if (total > 10)
        out << "    ... (bonsai-romeo, key !)\n";
    return out.str();
}

Component BonsaiErrors::window(std::shared_ptr<State> state, const UserScope* scope,
                               std::function<void(const std::string&)> on_open,
                               std::function<void()> on_close) {
    MenuOption option;

    option.entries_option.transform = [state, scope](const EntryState& entry) {
        if (entry.index < 0 || entry.index >= static_cast<int>(state->errors.size()))
            return text("");

        const auto& e = state->errors[entry.index];
        auto row = hbox({
            text(FormatUtils::trimSuffix(message(e.error), 24, "...")) | color(Color::RGB(255, 140, 0)) | size(WIDTH, EQUAL, 26),
            text(FormatUtils::trimMiddle(scope->display(e.path), pathWidth())) | flex,
        });

        return entry.active ? row | bgcolor(Color::White) | color(Color::Black) : row;
    };

    option.on_enter = [state, on_open] {
        if (state->selected >= 0 && state->selected < static_cast<int>(state->errors.size()))
            on_open(state->errors[state->selected].path);
    };

    auto menu = Menu(&state->labels, &state->selected, option);

    auto view = Renderer(menu, [state, menu] {
        std::string hint;
        if (state->selected >= 0 && state->selected < static_cast<int>(state->errors.size()))
            hint = advice(state->errors[state->selected].error);

        const std::string count = std::to_string(state->total) + (state->total == 1 ? " folder" : " folders");

        return vbox({
            hbox({
                text(" Folders that couldn't be read ") | bold,
                filler(),
                text(count + " ") | color(Color::GrayLight),
            }),
            paragraph(" What they hold isn't counted: the sizes above them (marked !) may be too low.") | color(Color::GrayLight),
            separator(),
            state->errors.empty() ? text(" None.") | color(Color::GrayLight)
                                  : menu->Render() | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 16),
            separator(),
            paragraph(" " + hint),
            text(" ENTER open in the explorer   ESC close") | color(Color::GrayDark),
        }) | border | size(WIDTH, LESS_THAN, 110);
    });

    return keepFocus(CatchEvent(view, [on_close](Event event) {
        if (event == Event::Escape || event == Event::Character('!')) {
            on_close();
            return true;
        }
        return false;
    }));
}
