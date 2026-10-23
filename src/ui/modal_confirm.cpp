#include "../../include/ui/modal_confirm.hpp"
#include "../../include/ui/theme.hpp"

#include <algorithm>

Component BonsaiModalConfirm::confirm(std::function<void ()> action, std::function<void ()> exit, const Text* text_, int* focused) {
    auto component = Container::Horizontal({
        Button("cancel", exit, ButtonOption::Ascii()) | bgcolor(Color::Default),
        Button(&text_->button, action, ButtonOption::Ascii()) | bgcolor(Color::Default) | color(Color::IndianRed),
    }, focused);

    component |= Renderer([text_](Element inner) {
        // One line per line of the target (the path, then the size)
        Elements lines;
        const std::string& target = text_->target;
        size_t start = 0;
        while (start <= target.size()) {
            const size_t end = std::min(target.find('\n', start), target.size());
            lines.push_back(text(target.substr(start, end - start)) | bold | center);
            start = end + 1;
        }

        // Red for a deletion, as RSV's scancel; packing deletes too, once the archive is checked
        const bool deleting = text_->button == "delete";
        const Color frame = deleting ? ui::theme::red() : ui::theme::yellow();
        std::string title = text_->button;
        if (!title.empty()) title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));

        return ui::overlay(ui::panel(title, vbox({
            text(""),
            text(text_->title) | center,
            vbox(std::move(lines)),
            text(text_->warning) | center | color(frame),
            text(""),
            inner | center,
            ui::keyHints({{"left/right", "choose"}, {"enter", "confirm"}, {"esc", "cancel"}}) | center,
            text(""),
        }) | size(WIDTH, GREATER_THAN, 70), frame));
    });
    return component;
}
