#include "../../include/ui/menu.hpp"
#include "../../include/ui/listing.hpp"

#include "../../include/config/config.hpp"
#include "../../include/ui/theme.hpp"
#include "../../include/utils/format.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

void BonsaiMenu::worker(ScreenInteractive* screen, std::shared_ptr<AppData::BonsaiData> data, Scanner* scanner, const fs::path& default_path, int* selected) {
    // What the folder shown holds, read once and kept (see Listing): only folder sizes change between passes
    Listing listing(scanner->getScope().uid);
    fs::path listed_path;

    while (true) {
        std::vector<AppData::BonsaiMenuEntry> new_entries;
        std::vector<std::string> new_labels;

        fs::path current_path;
        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            current_path = *data->path;
        }

        // Read before the sizes: if the scan ends during this pass, one more pass shows the final sizes
        const bool was_done = scanner->isDone();

        // Requests made after this point are for the next pass's list
        const uint64_t generation = data->list_generation.load();

        /* The folder shown was removed meanwhile (by another program, a job...)
        - Back to the closest folder above it that still exists (fs::equivalent() on it used to throw and crash)
        */
        std::error_code exists_ec;
        if (current_path != default_path && !fs::is_directory(current_path, exists_ec)) {
            fs::path up = current_path;
            while (up != default_path && up.has_parent_path() && up != up.parent_path() && !fs::is_directory(up, exists_ec))
                up = up.parent_path();

            if (!UserScope::isUnder(up.native(), default_path.native()) || scanner->getScope().relation(up.native()) != UserScope::Relation::INSIDE)
                up = default_path;

            // On the UI thread, and only if the user is still there
            screen->Post([data, gone = current_path, up] {
                {
                    std::lock_guard<std::mutex> lock(data->menu_mutex);
                    if (fs::path(*data->path) != gone)
                        return;
                    *data->path = up.string();
                }
                {
                    std::lock_guard<std::mutex> lock(data->cv_mutex);
                    data->pie_path_changed = true;
                }
                data->cv.notify_all();
            });
            current_path = up;
        }

        // A folder just opened is read again: what other programs changed in it since shows
        if (current_path != listed_path) {
            listing.expire(current_path, Listing::RECENT);
            listed_path = current_path;
        }

        // Always allow going back until default_path is reached
        if (current_path != default_path) {
            new_labels.push_back("..");
            new_entries.push_back(AppData::BonsaiMenuEntry{0, 0, "..", "", true});
        }

        try {
            std::vector<AppData::BonsaiMenuEntry> unsorted_entries;

            // At the top: the user's spaces, not the folders of GPFS above them
            const bool spaces_view = scanner->isSpacesView(current_path);

            if (spaces_view) {
                for (const auto& root : scanner->getScope().roots) {
                    const Scanner::Totals totals = scanner->totals(root);

                    AppData::BonsaiMenuEntry item;
                    item.path = root;
                    item.label = scanner->getScope().label(root);
                    item.is_dir = true;
                    item.size = scanner->value(totals);
                    item.final = totals.final;
                    item.known = totals.known;
                    item.error = totals.error;
                    item.newest = totals.newest;
                    item.snapped_size = scanner->getSnapped(root);
                    unsorted_entries.push_back(std::move(item));
                }
            }

            // What the folder holds, from the listing; folder sizes from the scan
            else for (const auto& entry : *listing.get(current_path, data->listing_version.load())) {
                // Other users' files and folders aren't shown
                if (!scanner->visibleEntry(entry.path, entry.is_dir, entry.owned))
                    continue;

                AppData::BonsaiMenuEntry item;
                item.path = entry.path;
                item.label = entry.name;
                item.is_dir = entry.is_dir;

                if (!item.is_dir) {
                    item.size = scanner->fileValue(entry.bytes, entry.disk);
                    item.newest = entry.mtime;
                    item.snapped_size = 0;
                } else {
                    const Scanner::Totals totals = scanner->totals(entry.path);
                    item.size = scanner->value(totals);
                    item.final = totals.final;
                    item.known = totals.known;
                    item.error = totals.error;
                    item.newest = totals.newest;
                    item.snapped_size = scanner->getSnapped(entry.path);
                }

                unsorted_entries.push_back(std::move(item));
            }

            /* Sort the aquired data (key S):
            - Directories go first
            - Then size descending (for the metric shown: F gives size, size on disk or files),
              or name (case aside), or age: the oldest change first, unknown dates last
            - Then name ascending
            */
            const AppData::Sort sort = data->sort.load();
            auto folded = [](const std::string& s) {
                std::string out(s);
                std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
                return out;
            };

            std::sort(unsorted_entries.begin(), unsorted_entries.end(), [sort, &folded](const AppData::BonsaiMenuEntry& a, const AppData::BonsaiMenuEntry& b) {
                if (a.is_dir != b.is_dir)
                    return a.is_dir;

                if (sort == AppData::Sort::NAME) {
                    const std::string fa = folded(a.label), fb = folded(b.label);
                    if (fa != fb)
                        return fa < fb;
                } else if (sort == AppData::Sort::AGE) {
                    if (a.newest != b.newest)
                        return b.newest == 0 || (a.newest != 0 && a.newest < b.newest);
                } else if (a.size != b.size) {
                    return a.size > b.size;
                }

                return a.label < b.label;
            });

            // Push the sorted data in the two array, in sync.
            for (auto& item : unsorted_entries) {
                new_labels.push_back(item.label);
                new_entries.push_back(std::move(item));
            }

        } catch (const fs::filesystem_error&) {}

        /* Published on the UI thread
        - FTXUI's Menu reads the labels while handling events (every mouse move too), outside of any lock:
          swapping them from this thread raced with it
        - The entry asked for (after F, going up, opening from search / cleanup) is looked up in this new
          list: looked up in the one on screen, it landed on the same row of the old list, another entry
        - A list for a folder the user already left is dropped: the next pass makes the right one
        */
        auto entries = std::make_shared<std::vector<AppData::BonsaiMenuEntry>>(std::move(new_entries));
        auto labels = std::make_shared<std::vector<std::string>>(std::move(new_labels));

        screen->Post([data, entries, labels, listed = current_path, selected, generation] {
            bool moved = false;
            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);
                if (fs::path(*data->path) != listed)
                    return;

                data->menu_entries->swap(*entries);
                data->menu_labels->swap(*labels);

                const int count = static_cast<int>(data->menu_entries->size());

                if (!data->select_path.empty() && fs::path(data->select_path) == listed && generation >= data->select_generation) {
                    int index = count > 1 && (*data->menu_entries)[0].label == ".." ? 1 : 0;
                    for (int i = 0; i < count && !data->select_label.empty(); i++) {
                        if ((*data->menu_entries)[i].label == data->select_label) {
                            index = i;
                            break;
                        }
                    }
                    *selected = index;
                    data->select_path.clear();
                    data->select_label.clear();
                    moved = true;
                }

                // The list got shorter (a deletion): stay on a row that exists
                if (*selected >= count) {
                    *selected = std::max(0, count - 1);
                    moved = true;
                }
                if (*data->selected != *selected) {
                    *data->selected = *selected;
                    moved = true;
                }
            }

            if (moved) {
                {
                    std::lock_guard<std::mutex> lock(data->cv_mutex);
                    data->pie_sel_changed = true;
                }
                data->cv.notify_all();
            }
        });

        // Update render and sleep if scan is finished or new path
        screen->PostEvent(Event::Custom);

        // Sleep until woken up or keep going until scanner has completed
        if(!was_done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(data->cv_mutex);

            data->cv.wait(lock, [&] {
                return data->menu_path_changed || data->stop;
            });

            if (data->stop)
                break;

            if (data->menu_path_changed)
                data->menu_path_changed = false;
        }
    }
}

Component BonsaiMenu::menu(ScreenInteractive* screen, std::shared_ptr<AppData::BonsaiData> data, Scanner* scanner, int* selected, const int* sidebar_width, const fs::path& default_path, MenuOption options) {

    // Selected or unselected colors
    options.entries_option.animated_colors.foreground = AnimatedColorOption{
        .inactive = Color::White,
        .active = Color::Default
    };

    // Height of the terminal in rows, refreshed once per frame (see the Renderer at the end)
    auto terminal_rows = std::make_shared<int>(Terminal::Size().dimy);

    // Plot the entry data on the menu in a nice way
    options.entries_option.transform = [data, scanner, selected, sidebar_width, terminal_rows](const EntryState& entry_state) {
        /* Performance:
        - FTXUI's Menu builds the element of every entry on every frame, not only of the ones on screen
        - With thousands of entries, building and laying out all these rows is what makes scrolling lag
        - The menu sits in a frame that keeps the selected entry in view, so an entry further away from
          the selection than the height of the terminal can't be visible
        - These entries get an empty line instead: same height, so the scrolling doesn't change
        */
        if (std::abs(entry_state.index - *selected) > *terminal_rows) {
            return text("");
        }

        const auto& item = (*data->menu_entries)[entry_state.index];
        const bool is_selected = entry_state.active;
        const bool is_marked = !data->marked.empty() && data->marked.count(item.path) > 0;
        const Config& config = Config::get();

        std::string icon_str;
        if (item.label == "..") {
            icon_str = config.SIDEBAR_BACK_ICON;
        } else if (is_marked) {
            icon_str = "*";
        } else {
            icon_str = is_selected ? (item.is_dir ? config.SIDEBAR_SELECTED_FOLDER_ICON : config.SIDEBAR_SELECTED_FILE_ICON)
                                   : (item.is_dir ? config.SIDEBAR_FOLDER_ICON          : config.SIDEBAR_FILE_ICON);
        }

        double growth = 0.0;
        std::string growth_str;

        if (item.is_dir && item.snapped_size != 0) {
            growth =
                (static_cast<double>(item.size) - static_cast<double>(item.snapped_size))
                / static_cast<double>(item.snapped_size)
                * 100.0;

            if (std::abs(growth) > config.SIDEBAR_GROWTH_THRESHOLD_PERCENTAGE) {
                std::ostringstream ss;

                const double abs_growth = std::abs(growth);

                ss << (growth > 0 ? "+" : "-");

                if (abs_growth >= 1'000'000'000'000.0) {
                    ss << std::fixed << std::setprecision(1)
                       << abs_growth / 1'000'000'000'000.0 << 'T';
                } else if (abs_growth >= 1'000'000'000.0) {
                    ss << std::fixed << std::setprecision(1)
                       << abs_growth / 1'000'000'000.0 << 'B';
                } else if (abs_growth >= 1'000'000.0) {
                    ss << std::fixed << std::setprecision(1)
                       << abs_growth / 1'000'000.0 << 'M';
                } else if (abs_growth >= 1'000.0) {
                    ss << std::fixed << std::setprecision(1)
                       << abs_growth / 1'000.0 << 'k';
                } else {
                    ss << std::fixed << std::setprecision(1)
                       << abs_growth;
                }

                ss << '%';

                growth_str = ss.str();
            }
        }

        /* Columns
        - Sized from the live sidebar width (it changes with the arrow keys), not the one in the config:
          the columns used to keep their first size and got cut when the sidebar was made narrower
        - Fixed widths that always add up to the sidebar (minus its right margin), so every row lines up
        */
        // Inside the panel's frame (two columns), one column kept free on the right
        const int width = std::max(*sidebar_width - 3, 12);
        const int size_width = 7;                         // "~" + toCompact / toCount (5 characters at most)
        const int growth_width = width >= 30 ? 8 : 0;     // " +12.3%"
        const int age_width = data->show_age && width >= 36 ? 6 : 0;   // " 1.4y"
        const int name_width = width - size_width - growth_width - age_width;

        const std::string age = item.label == ".." || item.newest <= 0 ? "" : FormatUtils::toAgeShort(std::time(nullptr) - item.newest);

        // "~": not final yet, from the previous scan or still being scanned. "?": not in the scan. "!": a folder in it couldn't be read
        const std::string mark = !item.known ? "" : item.error ? "!" : item.final ? "" : "~";

        auto row = hbox({
            text(" " + icon_str + " " + FormatUtils::trimSuffix(item.label, std::max(name_width - 4, 1), "...")) | size(WIDTH, EQUAL, name_width),

            hbox(
                filler(),
                text(item.label == ".." ? "" : growth_str) | color(growth > 0 ? Config::get().growthColor() : Config::get().shrinkColor())
            ) | size(WIDTH, EQUAL, growth_width),

            hbox(
                filler(),
                text(age) | dim
            ) | size(WIDTH, EQUAL, age_width),

            hbox(
                filler(),
                item.error ? text(item.label == ".." ? "" : mark) | color(ui::theme::yellow()) : text(item.label == ".." ? "" : mark) | dim,
                text(item.label == ".." ? "" : !item.known ? "?" : scanner->formatShort(item.size)) | dim
            ) | size(WIDTH, EQUAL, size_width)
        });

        // As in RSV: the text in the terminal's own color, the selection inverted; marked entries (SPACE) yellow
        if (is_selected)
            return row | inverted;
        return is_marked ? row | color(ui::theme::yellow()) : row;
    };

    // On selected: update data with a new path, and wake worker thread
    options.on_enter = [screen, data, selected, scanner, default_path]() {
        std::string new_path;
        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            if (*selected < 0 || *selected >= (int)data->menu_entries->size() || !(*data->menu_entries)[*selected].is_dir) {
                return;
            };

            new_path = (*data->menu_entries)[*selected].path;

            // Down: the new folder starts at its first entry
            std::string select = "";

            // For ".." go up; from a space, straight back to the list of spaces; the folder left is selected
            if ((*data->menu_entries)[*selected].label == "..") {
                std::filesystem::path p(*data->path);
                new_path = p.parent_path().string();
                select = p.filename().string();

                if (scanner->getScope().relation(new_path) != UserScope::Relation::INSIDE) {
                    new_path = default_path.string();
                    select = scanner->getScope().label(p.string());
                }
            }

            *data->selected = *selected;
            *data->path = new_path;
            data->select_path = new_path;
            data->select_label = select;
            data->select_generation = ++data->list_generation;
        }

        {
            // Wake up both pie and menu on change
            std::lock_guard<std::mutex> lock(data->cv_mutex);
            data->menu_path_changed = true;
            data->pie_path_changed = true;
        }

        data->cv.notify_all();
    };

    options.on_change = [data, selected]() {
        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            if (*selected < 0 || *selected >= (int)data->menu_entries->size()) {
                return;
            }

            *data->selected = *selected;
        }

        {
            /* Only wake up pie
            - Made so that it only partially re-renders / doesn't have to compute all children again
            */
            std::lock_guard<std::mutex> lock(data->cv_mutex);
            data->pie_sel_changed = true;
        }

        data->cv.notify_all();
    };

    // Menu is updated as labels update
    auto menu = Menu(data->menu_labels.get(), selected, options);

    // Same menu, the height of the terminal is just read before each frame is built
    return Renderer(menu, [menu, terminal_rows] {
        *terminal_rows = Terminal::Size().dimy;
        return menu->Render();
    });
}
