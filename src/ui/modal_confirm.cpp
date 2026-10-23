#include "../../include/ui/modal_confirm.hpp"

#include <algorithm>

Component BonsaiModalConfirm::confirm(std::function<void ()> action, std::function<void ()> exit, const std::string* target, int* focused) {
    auto component = Container::Horizontal({
        Button("cancel", exit, ButtonOption::Ascii()) | bgcolor(Color::Default),
        Button("delete", action, ButtonOption::Ascii()) | bgcolor(Color::Default) | color(Color::IndianRed),
    }, focused);

    component |= Renderer([target](Element inner) {
        // One line per line of `target` (the path, then the size)
        Elements lines;
        size_t start = 0;
        while (start <= target->size()) {
            const size_t end = std::min(target->find('\n', start), target->size());
            lines.push_back(text(target->substr(start, end - start)) | bold | center);
            start = end + 1;
        }

        return vbox({
            text(""),
            text("Delete this for good?") | center,
            vbox(std::move(lines)),
            text("ROMEO has no backups: this can't be undone.") | center | color(Color::IndianRed),
            text(""),
            inner | center,
            text("LEFT / RIGHT to choose, ENTER to confirm, ESC to cancel") | center | color(Color::GrayDark),
            text(""),

        }) | size(WIDTH, GREATER_THAN, 70) | border;
    });
    return component;
}
