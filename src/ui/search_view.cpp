#include "../../include/ui/search_view.hpp"

#include "../../include/ui/theme.hpp"
#include "../../include/utils/format.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <ctime>

using namespace ftxui;

namespace {

// Listed at most: enough to scroll through, cheap to sort
constexpr size_t MAX_HITS = 200;

// Room left for the path: the window is at most 120 columns, minus borders and the other columns
size_t pathWidth() {
    const int width = std::min(120, ftxui::Terminal::Size().dimx) - 2 - 22 - 1;
    return static_cast<size_t>(std::max(width, 12));
}

}

BonsaiSearch::Worker::Worker(std::shared_ptr<State> _state, Scanner* _scanner, std::function<void()> _on_result)
    : state(std::move(_state)), scanner(_scanner), on_result(std::move(_on_result)), thread(&Worker::run, this) {}

BonsaiSearch::Worker::~Worker() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
    }
    cv.notify_all();
    thread.join();
}

void BonsaiSearch::Worker::request(const std::string& query) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        wanted = query;
        requested++;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->searching = true;
    }
    cv.notify_all();
}

void BonsaiSearch::Worker::run() {
    uint64_t done = 0;

    while (true) {
        std::string query;
        uint64_t mine;

        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return stop || requested != done; });
            if (stop) return;

            query = wanted;
            mine = requested;
        }

        // Stopped as soon as a newer key comes in (or the program quits)
        auto cancelled = [this, mine] {
            std::lock_guard<std::mutex> lock(mutex);
            return stop || requested != mine;
        };

        size_t total = 0;
        auto hits = scanner->search(query, MAX_HITS, total, cancelled);
        done = mine;

        if (cancelled())
            continue;

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->hits = std::move(hits);
            state->total = total;
            state->version++;
            state->searching = false;
            state->partial = !scanner->isDone();
            state->now = std::time(nullptr);
        }

        if (on_result)
            on_result();
    }
}

Component BonsaiSearch::window(std::shared_ptr<State> state, Worker* worker, Scanner* scanner,
                               std::function<void(const std::string& path)> on_open,
                               std::function<void()> on_close) {
    InputOption option;
    option.placeholder = "folder name";
    option.multiline = false;
    option.on_change = [state, worker] { worker->request(state->query); };

    auto input = Input(&state->query, option);

    // The selection goes back to the top with every new list of hits
    auto seen_version = std::make_shared<uint64_t>(0);

    auto view = Renderer(input, [state, scanner, input, seen_version] {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (*seen_version != state->version) {
            *seen_version = state->version;
            state->selected = 0;
        }

        Elements rows;
        for (size_t i = 0; i < state->hits.size(); i++) {
            const auto& hit = state->hits[i];
            const std::string age = hit.totals.newest > 0 ? FormatUtils::toAge(state->now - hit.totals.newest) : "";

            auto row = hbox({
                text((hit.totals.final ? "" : "~") + scanner->formatShort(scanner->value(hit.totals))) | size(WIDTH, EQUAL, 7),
                text(FormatUtils::toCount(hit.totals.inodes)) | size(WIDTH, EQUAL, 7),
                text(age) | size(WIDTH, EQUAL, 8),
                text(FormatUtils::trimMiddle(scanner->getScope().display(hit.path), pathWidth())) | flex,
            });

            // `focus` keeps the selected row in view when the list scrolls
            rows.push_back(static_cast<int>(i) == state->selected ? row | inverted | focus : row);
        }

        std::string count;
        if (state->searching)
            count = "searching...";
        else if (!state->query.empty())
            count = state->total == 0 ? "no match"
                  : state->total > state->hits.size() ? std::to_string(state->hits.size()) + " biggest of " + std::to_string(state->total) + " matches"
                  : std::to_string(state->total) + (state->total == 1 ? " match" : " matches");

        Element list = rows.empty()
            ? paragraph(state->query.empty() ? " Type a folder name. Words are searched in the path, the last one in the folder name: \"adc build\" finds the build folders under adc..."
                                             : (state->searching ? " " : " No folder of yours matches.")) | dim
            : vbox(std::move(rows)) | vscroll_indicator | yframe | size(HEIGHT, LESS_THAN, 20);

        return ui::overlay(ui::panel("Search folders", vbox({
            hbox({text(" / ") | bold | color(ui::theme::accent()), input->Render() | flex}),
            state->partial ? text(" The scan is still running: more may show up") | color(ui::theme::yellow()) : emptyElement(),
            separator() | dim,
            hbox({
                text("SIZE") | size(WIDTH, EQUAL, 7),
                text("FILES") | size(WIDTH, EQUAL, 7),
                text("CHANGED") | size(WIDTH, EQUAL, 8),
                text("PATH"),
            }) | dim,
            list,
            separator() | dim,
            hbox({text(" "), ui::keyHints({{"enter", "open"}, {"up/down", "select"}, {"esc", "close"}})}),
        }), ui::theme::accent(), text(count) | dim) | size(WIDTH, LESS_THAN, 122));
    });

    return CatchEvent(view, [state, on_open, on_close](Event event) {
        if (event == Event::Escape) {
            // An ENTER still waiting would open a folder the next time the search is shown
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->open_when_ready = false;
            }
            on_close();
            return true;
        }

        std::string path;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const int count = static_cast<int>(state->hits.size());

            /* ENTER while the search still runs: remembered, the first hit opens once it is there
            - It used to do nothing, and the window stayed open: the next keys (Q too) went into the query
            */
            if (event == Event::Custom) {
                if (!state->open_when_ready || state->searching)
                    return false;
                state->open_when_ready = false;
                if (count == 0)
                    return false;
                path = state->hits[0].path;
            } else if (event == Event::Return) {
                if (state->searching) {
                    state->open_when_ready = true;
                    return true;
                }
                if (state->selected >= 0 && state->selected < count)
                    path = state->hits[state->selected].path;
                else
                    return true;
            } else if (count > 0 && event == Event::ArrowDown) { state->selected = std::min(state->selected + 1, count - 1); return true; }
            else if (count > 0 && event == Event::ArrowUp) { state->selected = std::max(state->selected - 1, 0); return true; }
            else if (count > 0 && event == Event::PageDown) { state->selected = std::min(state->selected + 10, count - 1); return true; }
            else if (count > 0 && event == Event::PageUp) { state->selected = std::max(state->selected - 10, 0); return true; }
            else {
                // Everything else is typing: a new query, so an ENTER waiting for the previous one no longer counts
                if (event.is_character() || event == Event::Backspace || event == Event::Delete)
                    state->open_when_ready = false;
                return false;
            }
        }

        // Outside of the lock: opening wakes the explorer threads
        on_open(path);
        return true;
    });
}
