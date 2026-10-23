#include "../include/ui/modal_confirm.hpp"
#include "../include/ui/modal_error.hpp"
#include "../include/config/config.hpp"
#include "../include/core/app_data.hpp"
#include "../include/core/scanner.hpp"
#include "../include/ui/piechart.hpp"
#include "../include/ui/menu.hpp"
#include "../include/ui/quota_view.hpp"
#include "../include/ui/cleanup_view.hpp"
#include "../include/ui/search_view.hpp"
#include "../include/utils/format.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <filesystem>
#include <ftxui/screen/terminal.hpp>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include <cstdio>
#include <iostream>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <poll.h>
#include <sys/stat.h>
#include <pwd.h>

/* TODO: bug? (can't reproduce anymore...)
-> scanner isn't finished.
-> menu scans for a folder, and updates sizes in list
-> this updates render, that updates piechart.
-> piechart worker doesn't wake up
-> for some reason percentages are updated but folder list isn't?

-> Maybe fixable with callback on scan completed (update render on complete) since this only happens when scanner is done
*/

namespace fs = std::filesystem;

/* Colors
- FTXUI decides between 24 bit and 256 colors itself, when its loop starts. TERM=xterm-256color
  makes it pick 256 colors, before it even looks at TERM_PROGRAM (WarpTerminal, ghostty...).
- Through SSH it can't do better: COLORTERM never reaches ROMEO (no environment variable does).
  So the pie lost most of its shades, even in terminals that have them.
- "auto" picks 24 bit colors for terminals known by TERM or TERM_PROGRAM, and for any SSH session
  in an xterm-like terminal: nearly every terminal used to reach ROMEO handles them.
- COLOR_MODE in the config, or BONSAI_COLOR, forces a mode. No value: FTXUI's choice stays.
*/
static std::optional<ftxui::Terminal::Color> chooseColors(std::string mode) {
    using ftxui::Terminal::Color;

    if (const char* env = std::getenv("BONSAI_COLOR"); env && *env)
        mode = env;

    if (mode == "truecolor" || mode == "24bit") return Color::TrueColor;
    if (mode == "256") return Color::Palette256;
    if (mode == "16") return Color::Palette16;

    auto env = [](const char* name) {
        const char* value = std::getenv(name);
        std::string s = value ? value : "";
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };

    const std::string colorterm = env("COLORTERM");
    const std::string term = env("TERM");
    const std::string program = env("TERM_PROGRAM");
    const bool ssh = !env("SSH_CONNECTION").empty() || !env("SSH_TTY").empty();

    if (colorterm == "truecolor" || colorterm == "24bit")
        return Color::TrueColor;

    for (const char* known : {"ghostty", "kitty", "wezterm", "alacritty", "foot", "direct", "iterm"})
        if (term.find(known) != std::string::npos)
            return Color::TrueColor;

    // Apple's Terminal.app before macOS 26 only has 256 colors
    if (program.find("apple_terminal") != std::string::npos)
        return Color::Palette256;

    for (const char* known : {"warp", "ghostty", "iterm", "wezterm", "vscode", "tabby", "hyper"})
        if (program.find(known) != std::string::npos)
            return Color::TrueColor;

    if (ssh && term.find("xterm") != std::string::npos)
        return Color::TrueColor;

    return std::nullopt;
}

int main(int argc, char* argv[]) {
    using namespace ftxui;

    bool summary = false;
    bool quota_only = false;
    bool cleanup_only = false;
    std::vector<std::string> args;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];

        if (arg == "--summary") {
            summary = true;
        } else if (arg == "--quota") {
            quota_only = true;
        } else if (arg == "--cleanup") {
            cleanup_only = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: bonsai-romeo [--summary | --quota | --cleanup] [path]\n"
                         "  Shows only your own data: home, scratch and your /project/<group> folders.\n"
                         "  --summary   scan, print size, disk usage and file count of each of your spaces, and exit\n"
                         "  --quota     print your GPFS quotas (space and files) with gauges, and exit\n"
                         "  --cleanup   scan, print what could be cleaned (caches, builds, old data, large files), and exit\n"
                         "  BONSAI_ROOTS=dir1:dir2   override the detected spaces\n"
                         "  BONSAI_THREADS=n         scan threads (default: automatic, up to 32)\n"
                         "  BONSAI_COLOR=truecolor|256|16   force the color mode (default: auto)\n"
                         "Keys: F size / disk / files, / search folders, C cleanup suggestions, U quotas, R refresh quotas,\n"
                         "      D (or SUPPR) delete, ENTER open, Backspace/ESC up, Q quit\n"
                         "Sizes marked ~ come from the previous scan (or are still growing) until the folder is scanned.\n";
            return 0;
        } else {
            args.push_back(arg);
        }
    }

    if (quota_only) {
        std::cout << BonsaiQuotaView::toText(Quota::fetch(), isatty(STDOUT_FILENO));
        return 0;
    }

    // The user's spaces (home, scratch, projects), see UserScope
    UserScope scope = UserScope::detect();

    fs::path default_path = args.empty() ? scope.commonAncestor() : fs::weakly_canonical(args.front());

    // One spelling for it (no trailing slash): paths are compared as they are written
    {
        std::string normal = default_path.lexically_normal().string();
        while (normal.size() > 1 && normal.back() == '/')
            normal.pop_back();
        default_path = normal;
    }

    if(!fs::exists(default_path)) {
        std::cerr << "Error: Path does not exist: " << default_path << std::endl;
        return 1;
    }

    if(!fs::is_directory(default_path)) {
        std::cerr << "Error: Path isn't a directory: " << default_path << std::endl;
        return 1;
    }

    // A path outside of the detected spaces becomes the only space (still only your own files)
    if (scope.relation(default_path.native()) == UserScope::Relation::OUTSIDE) {
        scope.roots = {default_path.string()};
    }

    // Above some spaces only (bonsai-romeo /project): the others aren't listed (nor scanned)
    if (scope.relation(default_path.native()) == UserScope::Relation::ANCESTOR) {
        std::vector<std::string> below;
        for (const auto& root : scope.roots) {
            if (UserScope::isUnder(root, default_path.native()))
                below.push_back(root);
        }
        if (!below.empty())
            scope.roots = below;
    }

    // Launch scanner
    auto home = fs::path(getpwuid(getuid())->pw_dir);
    // BONSAI_CACHE: another cache file (tests, or several setups)
    const char* cache_env = std::getenv("BONSAI_CACHE");
    auto db_path = fs::weakly_canonical(cache_env && *cache_env ? fs::path(cache_env) : home / ".cache/bonsai-romeo/db.txt");

    if (cleanup_only) {
        Scanner scanner = Scanner(default_path, db_path, scope);
        scanner.scan();
        std::cout << BonsaiCleanup::toText(scanner.cleanup(), scope, 40);
        scanner.snapshot();
        return 0;
    }

    if (summary) {
        Scanner scanner = Scanner(default_path, db_path, scope);
        scanner.scan();

        // Each space below the start path, or the start path itself if it is inside a space
        std::vector<std::string> shown;
        for (const auto& root : scope.roots) {
            if (UserScope::isUnder(root, default_path.native()))
                shown.push_back(root);
        }
        if (shown.empty())
            shown.push_back(default_path.string());

        Scanner::Totals total;
        for (const auto& path : shown) {
            const Scanner::Totals t = scanner.totals(path);
            total.bytes += t.bytes;
            total.disk += t.disk;
            total.inodes += t.inodes;

            std::printf("%10s %10s on disk %10s files   %s\n", FormatUtils::toReadableShort(t.bytes).c_str(), FormatUtils::toReadableShort(t.disk).c_str(),
                        FormatUtils::toCount(t.inodes).c_str(), scope.display(path).c_str());
        }
        std::printf("%10s %10s on disk %10s files   total (%u directories)\n", FormatUtils::toReadableShort(total.bytes).c_str(), FormatUtils::toReadableShort(total.disk).c_str(),
                    FormatUtils::toCount(total.inodes).c_str(), scanner.directories());

        scanner.snapshot();
        return 0;
    }

    auto config = Config::get();

    // Set now for what is drawn before the loop starts, and again once FTXUI has made its own choice (see below)
    const auto colors = chooseColors(config.COLOR_MODE);
    if (colors)
        ftxui::Terminal::SetColorSupport(*colors);

    auto screen = ScreenInteractive::Fullscreen();

    // GPFS quotas, read in the background (key U)
    QuotaMonitor quotas;
    quotas.onUpdate([&screen] { screen.PostEvent(Event::Custom); });
    bool show_quota = false;
    bool show_cleanup = false;
    bool show_search = false;

    Scanner scanner = Scanner(default_path, db_path, scope);
    std::thread scanner_thread([&scanner]() {
        scanner.scan();
        scanner.snapshot();
    });

    // Init shared menu & pie data
    int selected = 0;
    auto data = std::make_shared<AppData::BonsaiData>();

    data->menu_entries = std::make_shared<std::vector<AppData::BonsaiMenuEntry>>();
    data->menu_labels = std::make_shared<std::vector<std::string>>();

    data->path = std::make_shared<std::string>(default_path);
    data->selected = std::make_shared<int>(0);

    /* Init confirm & error modals
    - Allow quitting modal with button
    - Allow quitting modal with escape key
    */
    bool show_confirm_modal = false;
    bool show_error_modal = false;
    Scanner::ScannerRemoveResult result = Scanner::ScannerRemoveResult{"", false};

    /* Deletion
    - D asks first, showing what and how much; the window starts on "cancel"
    - Removing a big folder from GPFS can take minutes: it runs on its own thread, the explorer stays usable
    - Only one at a time; quitting waits for it to finish
    */
    std::string confirm_target;   // Shown in the confirm window
    std::string confirm_path;     // What D asked to delete
    int confirm_focus = 0;        // 0: cancel, 1: delete
    std::atomic<bool> deleting{false};
    std::string deleting_label;   // UI thread only
    std::string notice;           // "deleted ...", shown under the title for a few seconds (UI thread only)
    auto notice_until = std::chrono::steady_clock::now();
    std::thread delete_thread;

    auto confirm_modal = BonsaiModalConfirm::confirm(
        [&]() {
            show_confirm_modal = false;
            if (confirm_path.empty() || deleting)
                return;

            if (delete_thread.joinable())
                delete_thread.join();

            deleting = true;
            deleting_label = scope.display(confirm_path);

            delete_thread = std::thread([&, path = confirm_path] {
                const Scanner::ScannerRemoveResult removed = scanner.remove(path);

                // Back on the UI thread
                screen.Post([&, removed] {
                    deleting = false;
                    result = removed;
                    data->listing_version++;
                    if (result.failed) {
                        show_error_modal = true;
                    } else {
                        notice = "deleted " + deleting_label + (removed.reason.empty() ? "" : " (" + removed.reason + ")");
                        notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                        screen.PostEvent(Event::Custom);
                    }

                    {
                        // Wake up both pie and menu on change
                        std::lock_guard<std::mutex> lock(data->cv_mutex);
                        data->menu_path_changed = true;
                        data->pie_path_changed = true;
                    }
                    data->cv.notify_all();
                });
                screen.PostEvent(Event::Custom);
            });
        },
        [&]{
            show_confirm_modal = false;
        },
        &confirm_target,
        &confirm_focus
    );

    confirm_modal = CatchEvent(confirm_modal, [&show_confirm_modal](Event event) {
        if(event == Event::Escape) {
            show_confirm_modal = false;
            return true;
        }

        return false;
    });

    auto error_modal = BonsaiModalError::error(result.reason,
        [&show_error_modal](){
            show_error_modal = false;
        }
    );

    error_modal = CatchEvent(error_modal, [&show_error_modal](Event event) {
        if(event == Event::Escape) {
            show_error_modal = false;
            return true;
        }

        return false;
    });

    /* Init menu:
    - Use a container to keep focus through the entire render.
      If a menu component is embeded in a slew of elements, it becomes static
    - Allow going back in tree with escape or backspace keys
    */
    MenuOption option;

    auto menu_component = BonsaiMenu::menu(&screen, data, &scanner, &selected, &config.SIDEBAR_WIDTH, default_path, option);
    menu_component = CatchEvent(menu_component, [&](Event event) {
        // Mac keyboards have no SUPPR key (their delete key is Backspace, which goes up a folder): D works everywhere
        if (event == Event::Delete || event == Event::Character('d') || event == Event::Character('D')) {
            if (deleting)
                return true;

            std::string path;
            bool is_dir = false;
            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);
                // The Menu's own index: the shared copy may lag one list behind
                if (selected >= 0 && selected < static_cast<int>(data->menu_entries->size())) {
                    path = (*data->menu_entries)[selected].path;
                    is_dir = (*data->menu_entries)[selected].is_dir;
                }
            }

            // ".." has no path
            if (path.empty())
                return true;

            // A whole space (all of home, all of scratch) is never one key away
            if (scope.isRoot(path)) {
                result = Scanner::ScannerRemoveResult{"This is one of your spaces: open it and delete what is inside instead.", true};
                show_error_modal = true;
                return true;
            }

            const Scanner::Totals totals = scanner.totals(path);
            std::string amount;
            if (is_dir) {
                // "~": not final yet (from the previous scan, or still being scanned)
                const std::string about = totals.final ? "" : "~";
                amount = about + FormatUtils::toReadable(totals.disk, " ") + " on disk, " + about + FormatUtils::toCount(totals.inodes, " ") + " files";
            } else {
                // fs::file_size() gives uintmax_t(-1) on error, shown as 17179869184 GiB
                struct stat st;
                amount = lstat(path.c_str(), &st) == 0 ? FormatUtils::toReadable(static_cast<uint64_t>(st.st_blocks) * 512, " ") + " on disk" : "size unknown";
            }

            // The path cut in its middle to the screen (its end, the name, stays), the size on its own line
            const size_t room = static_cast<size_t>(std::max(20, std::min(ftxui::Terminal::Size().dimx, 100) - 8));
            confirm_path = path;
            confirm_target = FormatUtils::trimMiddle(scope.display(path), room) + "\n(" + amount + ")";
            confirm_focus = 0;
            show_confirm_modal = true;
            return true;
        }

        if(event == Event::Backspace || event == Event::Escape) {
            std::string new_path;
            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);

                if(fs::path(*data->path) == default_path) {
                    return true;
                }

                std::filesystem::path p(*data->path);
                new_path = p.parent_path().string();
                std::string select = p.filename().string();

                // From a space, straight back to the list of spaces (not GPFS's folders above it)
                if (scope.relation(new_path) != UserScope::Relation::INSIDE) {
                    new_path = default_path.string();
                    select = scope.label(p.string());
                }

                // The folder just left is selected in its parent
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
            return true;
        }

        return false;
    });

    auto menu_container = Container::Vertical({menu_component});
    std::thread menu_thread(BonsaiMenu::worker, &screen, data, &scanner, default_path, &selected);


    /* Init pie:
    - No need for a containter this time, as the component isn't interactive
    */
    data->pie_entries = std::make_shared<std::vector<AppData::BonsaiPieEntry>>();

    auto pie_component = BonsaiPie::pie(data, &scanner, default_path);
    std::thread pie_thread(BonsaiPie::worker, &screen, data, &scanner, default_path);

    auto left = Renderer(menu_container, [&] {
        std::lock_guard<std::mutex> lock_m(data->menu_mutex);

        const Scanner::Metric metric = scanner.getMetric();

        // Where the explorer is, the way the user knows it
        const bool noticing = !notice.empty() && std::chrono::steady_clock::now() < notice_until;
        const std::string here = deleting ? "deleting " + deleting_label + "..."
                               : noticing ? notice
                               : scanner.isSpacesView(*data->path) ? "your spaces" : scope.display(*data->path);
        const size_t here_width = static_cast<size_t>(std::max(config.SIDEBAR_WIDTH - 2, 8));
        const char* metric_label = metric == Scanner::Metric::INODES ? " [files]" : (metric == Scanner::Metric::DISK ? " [disk]" : " [size]");

        return vbox({
            text(" "),
            hbox({
                text("Explorer") | bold,
                text(metric_label) | color(Color::GrayDark),
                text(" "),
                BonsaiQuotaView::badge(quotas.get(), quotas.ready()),
            }) | center,
            text(" " + FormatUtils::trimMiddle(here, here_width)) | color(deleting ? Color::RGB(255, 140, 0) : noticing ? Color::RGB(60, 179, 113) : Color::GrayLight),
            separator(),
            hbox({
                menu_component->Render() | frame | flex,
                text(" ")
            }) | flex,
            separator(),
            text(" F size  C clean  / find  U quota") | color(Color::GrayDark),
            text(" D delete  Q quit") | color(Color::GrayDark),
        });
    });

    // Right side: the pie chart, or the quotas
    auto right = Renderer(pie_component, [&] {
        if (show_quota)
            return BonsaiQuotaView::render(quotas.get(), quotas.ready(), ftxui::Terminal::Size().dimx - config.SIDEBAR_WIDTH - 1);

        return pie_component->Render();
    });

    auto window = ResizableSplitLeft(left, right, &config.SIDEBAR_WIDTH);

    /* Opening a path in the explorer (from the cleanup or the search window)
    - Goes to its folder with it selected, so ENTER opens it and D deletes it
    */
    auto open_in_explorer = [&](const std::string& path) {
        fs::path target(path);
        fs::path folder = target.parent_path();
        std::string label = target.filename().string();

        // A space itself: selected in the list of spaces
        if (scope.isRoot(target.string()) && scanner.isSpacesView(default_path)) {
            folder = default_path;
            label = scope.label(target.string());
        }

        // Never above where the explorer starts
        if (!UserScope::isUnder(folder.native(), default_path.native())) {
            folder = target;
            label.clear();
        }

        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            *data->path = folder.string();
            data->select_path = folder.string();
            data->select_label = label;
            data->select_generation = ++data->list_generation;
        }

        {
            std::lock_guard<std::mutex> lock(data->cv_mutex);
            data->menu_path_changed = true;
            data->pie_path_changed = true;
        }
        data->cv.notify_all();

        show_quota = false;
    };

    // Cleanup suggestions (key C)
    auto cleanup_state = std::make_shared<BonsaiCleanup::State>();
    std::thread cleanup_thread;
    std::atomic<bool> cleanup_running{false};
    auto cleanup_component = BonsaiCleanup::window(cleanup_state, &scanner.getScope(),
        [&](const Scanner::CleanupItem& item) {
            open_in_explorer(item.path);
            show_cleanup = false;
        },
        [&] { show_cleanup = false; });

    // Folder search (key /), searched on its own thread
    auto search_state = std::make_shared<BonsaiSearch::State>();
    BonsaiSearch::Worker search_worker(search_state, &scanner, [&screen] { screen.PostEvent(Event::Custom); });
    auto search_component = BonsaiSearch::window(search_state, &search_worker, &scanner,
        [&](const std::string& path) {
            open_in_explorer(path);
            show_search = false;
        },
        [&] { show_search = false; });

    window |= Modal(confirm_modal, &show_confirm_modal);
    window |= Modal(error_modal, &show_error_modal);
    window |= Modal(cleanup_component, &show_cleanup);
    window |= Modal(search_component, &show_search);

    auto modal_open = [&] { return show_confirm_modal || show_error_modal || show_cleanup || show_search; };

    // Quit with q, allow sidebar resize with arrow keys
    auto app = CatchEvent(window, [&](Event event) {
        // Not while typing a search
        if ((event == Event::Character('q') || event == Event::Character('Q')) && !show_search) {
            screen.Exit();
            return true;
        }

        if (event == Event::Character('/') && !modal_open()) {
            search_worker.request(search_state->query);
            show_search = true;
            return true;
        }

        if ((event == Event::Character('u') || event == Event::Character('U')) && !modal_open()) {
            show_quota = !show_quota;
            if (show_quota) quotas.refresh();
            return true;
        }

        if ((event == Event::Character('r') || event == Event::Character('R')) && !modal_open()) {
            quotas.refresh();
            return true;
        }

        // Suggestions walk the whole tree (~700k folders): on their own thread, the window opens at once
        if ((event == Event::Character('c') || event == Event::Character('C')) && !modal_open()) {
            show_cleanup = true;

            // Still looking (C, ESC, C): its list shows when it is ready, instead of the UI waiting for it here
            if (cleanup_running)
                return true;
            if (cleanup_thread.joinable())
                cleanup_thread.join();

            cleanup_state->computing = true;
            cleanup_state->all.clear();
            BonsaiCleanup::apply(*cleanup_state);

            cleanup_running = true;
            cleanup_thread = std::thread([&] {
                auto items = std::make_shared<std::vector<Scanner::CleanupItem>>(scanner.cleanup());
                const bool partial = !scanner.isDone();

                screen.Post([&, items, partial] {
                    BonsaiCleanup::set(*cleanup_state, std::move(*items), partial);
                });
                screen.PostEvent(Event::Custom);
                cleanup_running = false;
            });
            return true;
        }

        // Size, size on disk, file count: the menu and the pie are listed again with the next metric
        if ((event == Event::Character('f') || event == Event::Character('F')) && !modal_open()) {
            const Scanner::Metric metric = scanner.getMetric();
            scanner.setMetric(metric == Scanner::Metric::BYTES ? Scanner::Metric::DISK
                            : metric == Scanner::Metric::DISK ? Scanner::Metric::INODES : Scanner::Metric::BYTES);

            // The list is sorted again: the same entry stays selected (looked up in the new list)
            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);
                if (selected >= 0 && selected < static_cast<int>(data->menu_entries->size())) {
                    data->select_path = *data->path;
                    data->select_label = (*data->menu_entries)[selected].label;
                    data->select_generation = ++data->list_generation;
                }
            }

            {
                std::lock_guard<std::mutex> lock(data->cv_mutex);
                data->menu_path_changed = true;
                data->pie_path_changed = true;
            }

            data->cv.notify_all();
            return true;
        }

        if(event == Event::ArrowRight && !modal_open()) {
            config.SIDEBAR_WIDTH = std::min(config.SIDEBAR_WIDTH + 1, std::max(20, ftxui::Terminal::Size().dimx - 20));
            return true;
        }

        if(event == Event::ArrowLeft && !modal_open()) {
            config.SIDEBAR_WIDTH = std::max(20, config.SIDEBAR_WIDTH - 1);
            return true;
        }

        return false;
    });

    /* Colors, again
    - FTXUI picks its own color mode when the loop starts (and again after a suspend), see chooseColors().
    - Checked before each frame: when it differs, ours is put back and the pie is computed again,
      since its colors were already rounded to the wrong mode.
    */
    // The width the user picked (keys, mouse), and the one last drawn: a narrow terminal only narrows it on screen
    int sidebar_width = config.SIDEBAR_WIDTH;
    int shown_width = config.SIDEBAR_WIDTH;

    auto root = Renderer(app, [&] {
        /* Sidebar width
        - Arrow keys and mouse drags could push it past the screen: the chart vanished, and the width was saved so
        - Kept between 20 columns and the terminal width minus 20 (for the chart)
        */
        if (config.SIDEBAR_WIDTH != shown_width)
            sidebar_width = config.SIDEBAR_WIDTH;
        config.SIDEBAR_WIDTH = shown_width = std::clamp(sidebar_width, 20, std::max(20, ftxui::Terminal::Size().dimx - 20));

        if (colors && ftxui::Terminal::ColorSupport() != *colors) {
            ftxui::Terminal::SetColorSupport(*colors);

            {
                std::lock_guard<std::mutex> lock(data->cv_mutex);
                data->pie_path_changed = true;
            }
            data->cv.notify_all();
        }

        return app->Render();
    });

    /* Hangup watchdog
    - When the SSH connection drops, FTXUI keeps its loop running on a terminal that is gone,
      and the program stays on the login node forever.
    - The terminal reports the hangup on stdin: the loop is then asked to stop, like with Q.
    */
    std::atomic<bool> running{true};
    std::thread hangup_thread([&screen, &running] {
        while (running) {
            pollfd input{STDIN_FILENO, 0, 0};

            if (poll(&input, 1, 0) > 0 && (input.revents & (POLLHUP | POLLERR | POLLNVAL))) {
                screen.Exit();
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    // Render UI
    screen.Loop(root);
    running = false;
    hangup_thread.join();

    if (cleanup_thread.joinable())
        cleanup_thread.join();

    if (delete_thread.joinable()) {
        if (deleting)
            std::cerr << "Waiting for the deletion of " << deleting_label << " to finish..." << std::endl;
        delete_thread.join();
    }


    // Stop app
    scanner.stop();
    AppData::stop(data);

    // Save config: the width picked, not the one a small terminal forced
    if (config.SIDEBAR_WIDTH != shown_width)
        sidebar_width = config.SIDEBAR_WIDTH;
    config.SIDEBAR_WIDTH = sidebar_width;
    config.writeToFile();

    scanner_thread.join();
    menu_thread.join();
    pie_thread.join();

    return 0;
}
