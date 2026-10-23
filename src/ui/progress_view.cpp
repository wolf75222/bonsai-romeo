#include "../../include/ui/progress_view.hpp"

#include "../../include/ui/focus.hpp"
#include "../../include/ui/theme.hpp"
#include "../../include/utils/format.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <chrono>

using namespace ftxui;

namespace {

// Entries shown live under the bar: always as many lines, so the window doesn't jump
constexpr size_t RECENT = 6;

int windowWidth() {
    return std::clamp(ftxui::Terminal::Size().dimx - 8, 50, 110);
}

double ratioOf(uint64_t done, uint64_t total) {
    return total == 0 ? 0 : std::min(1.0, static_cast<double>(done) / static_cast<double>(total));
}

}

std::string BonsaiProgress::percent(Progress& progress) {
    const uint64_t total = progress.total.load();
    if (total == 0)
        return "";
    return std::to_string(static_cast<int>(ratioOf(progress.done.load(), total) * 100)) + "%";
}

Component BonsaiProgress::window(const Job* job, const UserScope* scope,
                                 std::function<void()> on_hide, std::function<void()> on_stop) {
    auto view = Renderer([job, scope] {
        if (!job->progress)
            return text("");

        Progress& p = *job->progress;

        // Over: how it ended, a full bar
        if (job->finished) {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - p.started).count();
            const int width = windowWidth();
            return ui::overlay(ui::panel(FormatUtils::trimMiddle(job->title, static_cast<size_t>(width - 20)), vbox({
                text(" Done") | color(ui::theme::green()),
                text(""),
                hbox({text(" "), gauge(1.0f) | color(ui::theme::green()) | flex, text(" 100% ") | bold}),
                paragraph(" " + job->result),
                text(""),
                separator() | dim,
                hbox({text(" "), ui::keyHints({{"esc", "close"}})}),
            }), ui::theme::green(), text(FormatUtils::toDuration(elapsed)) | dim) | size(WIDTH, EQUAL, width));
        }

        const uint64_t done = p.done.load();
        const uint64_t total = p.total.load();
        const double seconds = p.stepSeconds();
        const bool stopping = p.stop.load();
        const int width = windowWidth();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - p.started).count();

        Elements rows;
        rows.push_back(text(" " + p.stepName() + (stopping ? ": stopping..." : "")) | (stopping ? color(ui::theme::yellow()) : dim));
        rows.push_back(text(""));

        // The bar, when the scan says how many entries there are
        if (total > 0) {
            const double ratio = ratioOf(done, total);
            rows.push_back(hbox({
                text(" "),
                gauge(static_cast<float>(ratio)) | color(ui::theme::green()) | flex,
                text(" " + std::to_string(static_cast<int>(ratio * 100)) + "% ") | bold,
            }));

            std::string amount = FormatUtils::withSeparators(done) + " / " + FormatUtils::withSeparators(std::max(done, total)) + " entries";
            if (job->deleting && p.disk.load() > 0)
                amount += " · ~" + FormatUtils::toReadable(static_cast<uint64_t>(static_cast<double>(p.disk.load()) * ratio), " ")
                        + " of " + FormatUtils::toReadable(p.disk.load(), " ") + " freed";
            rows.push_back(text(" " + amount));
        } else if (p.checked.load() > 0 && done == 0) {
            rows.push_back(text(" " + FormatUtils::withSeparators(p.checked.load()) + " folders checked"));
        } else {
            rows.push_back(text(done > 0 ? " " + FormatUtils::withSeparators(done) + " entries" : " "));
        }

        // How fast, and about how long is left, once it can be told
        std::string pace = " ";
        if (seconds >= 1 && done > 0) {
            const double rate = static_cast<double>(done) / seconds;
            pace = " " + FormatUtils::toCount(static_cast<uint64_t>(rate)) + " per second";
            if (total > done)
                pace += " · about " + FormatUtils::toDuration(static_cast<double>(total - done) / rate) + " left";
        }
        rows.push_back(text(pace) | dim);

        // The entries it is on, live
        rows.push_back(separator() | dim);
        const std::vector<std::string> recent = p.recent(RECENT);
        for (size_t i = 0; i < RECENT; i++) {
            const std::string line = i < recent.size() ? " " + FormatUtils::trimMiddle(scope->display(recent[i]), static_cast<size_t>(width - 4)) : " ";
            rows.push_back(i == 0 ? text(line) : text(line) | dim);
        }
        rows.push_back(separator() | dim);
        rows.push_back(hbox({text(" "), stopping ? ui::keyHints({{"esc", "hide"}})
                                                 : ui::keyHints({{"esc", "hide (it keeps going)"}, {"s", "stop (what is done stays done)"}})}));

        return ui::overlay(ui::panel(FormatUtils::trimMiddle(job->title, static_cast<size_t>(width - 20)), vbox(std::move(rows)),
                                     stopping ? ui::theme::yellow() : ui::theme::accent(), text(FormatUtils::toDuration(elapsed)) | dim)
                           | size(WIDTH, EQUAL, width));
    });

    return keepFocus(CatchEvent(view, [on_hide, on_stop](Event event) {
        if (event == Event::Escape) {
            on_hide();
            return true;
        }
        if (event == Event::Character('s') || event == Event::Character('S')) {
            on_stop();
            return true;
        }
        return false;
    }));
}
