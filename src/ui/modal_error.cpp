#include "../../include/ui/modal_error.hpp"
#include "../../include/ui/theme.hpp"

#include <algorithm>

Component BonsaiModalError::error(const std::string* title, const std::string* message, std::function<void ()> exit) {
    auto component = Container::Horizontal({
        Button("exit", exit, ButtonOption::Ascii()) | bgcolor(Color::Default),
    });

    component |= Renderer([title, message](Element inner) {
        // Several failures (deleting marked entries): one line each, wrapped to the window
        Elements lines;
        size_t start = 0;
        while (start <= message->size()) {
            const size_t end = std::min(message->find('\n', start), message->size());
            lines.push_back(paragraph(" " + message->substr(start, end - start) + " "));
            start = end + 1;
        }

        return ui::overlay(ui::panel(*title, vbox({
            text(""),
            vbox(std::move(lines)),
            text(""),
            inner | center,
            text(""),
        }) | size(WIDTH, GREATER_THAN, 70) | size(WIDTH, LESS_THAN, 110), ui::theme::red()));
    });
    return component;
}
