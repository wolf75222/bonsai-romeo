#include "../../include/ui/modal_confirm.hpp"

Component BonsaiModalConfirm::confirm(std::function<void ()> action, std::function<void ()> exit, const std::string* target, int* focused) {
    auto component = Container::Horizontal({
        Button("cancel", exit, ButtonOption::Ascii()) | bgcolor(Color::Default),
        Button("delete", action, ButtonOption::Ascii()) | bgcolor(Color::Default) | color(Color::IndianRed),
    }, focused);

    component |= Renderer([target](Element inner) {
        return vbox({
            text(""),
            text("Delete this for good?") | center,
            text(*target) | bold | center,
            text("ROMEO has no backups: this can't be undone.") | center | color(Color::IndianRed),
            text(""),
            inner | center,
            text("LEFT / RIGHT to choose, ENTER to confirm, ESC to cancel") | center | color(Color::GrayDark),
            text(""),

        }) | size(WIDTH, GREATER_THAN, 70) | border;
    });
    return component;
}
