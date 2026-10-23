#include "../include/ui/modal_confirm.hpp"
#include "../include/ui/modal_error.hpp"
#include "../include/config/config.hpp"
#include "../include/core/app_data.hpp"
#include "../include/core/scanner.hpp"
#include "../include/ui/piechart.hpp"
#include "../include/ui/menu.hpp"
#include "../include/ui/quota_view.hpp"
#include "../include/utils/format.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <filesystem>
#include <ftxui/screen/terminal.hpp>
#include <cstdlib>
#include <string>
#include <vector>

#include <iostream>
#include <unistd.h>
#include <thread>
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
- FTXUI only uses 24 bit colors when COLORTERM says so, else it falls back to 256 colors.
- SSH doesn't carry COLORTERM (ROMEO accepts no environment variable at all), so through SSH
  the pie chart lost most of its shades, even in a terminal that has them.
- "auto" therefore also picks truecolor for terminals known by their TERM, and for any SSH session
  in an xterm-like terminal: nearly every terminal used to reach ROMEO handles 24 bit colors.
- COLOR_MODE in the config, or BONSAI_COLOR, forces a mode.
*/
static void setupColors(std::string mode) {
    using ftxui::Terminal::SetColorSupport;

    if (const char* env = std::getenv("BONSAI_COLOR"); env && *env)
        mode = env;

    if (mode == "truecolor" || mode == "24bit") return SetColorSupport(ftxui::Terminal::Color::TrueColor);
    if (mode == "256") return SetColorSupport(ftxui::Terminal::Color::Palette256);
    if (mode == "16") return SetColorSupport(ftxui::Terminal::Color::Palette16);

    auto env = [](const char* name) {
        const char* value = std::getenv(name);
        return std::string(value ? value : "");
    };

    const std::string colorterm = env("COLORTERM");
    const std::string term = env("TERM");
    const bool ssh = !env("SSH_CONNECTION").empty() || !env("SSH_TTY").empty();

    if (colorterm == "truecolor" || colorterm == "24bit")
        return SetColorSupport(ftxui::Terminal::Color::TrueColor);

    for (const char* known : {"ghostty", "kitty", "wezterm", "alacritty", "foot", "direct", "iterm"})
        if (term.find(known) != std::string::npos)
            return SetColorSupport(ftxui::Terminal::Color::TrueColor);

    if (ssh && term.find("xterm") != std::string::npos)
        return SetColorSupport(ftxui::Terminal::Color::TrueColor);

    // Otherwise FTXUI's own detection stays
}

int main(int argc, char* argv[]) {
    using namespace ftxui;

    bool summary = false;
    bool quota_only = false;
    std::vector<std::string> args;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];

        if (arg == "--summary") {
            summary = true;
        } else if (arg == "--quota") {
            quota_only = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: bonsai-romeo [--summary | --quota] [path]\n"
                         "  Shows only your own data: home, scratch and your /project/<group> folders.\n"
                         "  --summary   scan, print size and file count of each of your spaces, and exit\n"
                         "  --quota     print your GPFS quotas (space and files) with gauges, and exit\n"
                         "  BONSAI_ROOTS=dir1:dir2   override the detected spaces\n"
                         "  BONSAI_COLOR=truecolor|256|16   force the color mode (default: auto)\n"
                         "Keys: U quotas, F size / file count, R refresh quotas, D (or SUPPR) delete, Backspace/ESC up, Q quit\n";
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

    auto default_path = args.empty() ? scope.commonAncestor() : fs::weakly_canonical(args.front());

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

    // Launch scanner
    auto home = fs::path(getpwuid(getuid())->pw_dir);
    auto db_path = fs::weakly_canonical(home / ".cache/bonsai-romeo/db.txt");

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

        uint64_t total = 0;
        uint64_t total_inodes = 0;
        for (const auto& path : shown) {
            const uint64_t size = scanner.getBytes(path);
            const uint64_t inodes = scanner.getInodes(path);
            total += size;
            total_inodes += inodes;
            std::cout << FormatUtils::toReadableShort(size) << "\t" << FormatUtils::toCount(inodes) << " files\t" << path << "\n";
        }
        std::cout << FormatUtils::toReadableShort(total) << "\t" << FormatUtils::toCount(total_inodes) << " files\ttotal\n";

        scanner.snapshot();
        return 0;
    }

    auto config = Config::get();
    setupColors(config.COLOR_MODE);

    auto screen = ScreenInteractive::Fullscreen();

    // GPFS quotas, read in the background (key U)
    QuotaMonitor quotas;
    quotas.onUpdate([&screen] { screen.PostEvent(Event::Custom); });
    bool show_quota = false;

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

    auto confirm_modal = BonsaiModalConfirm::confirm(
        [data, &scanner, &result, &show_confirm_modal, &show_error_modal](){
            fs::path selected_path;

            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);
                selected_path = (*data->menu_entries)[*data->selected].path;
            }

            result = scanner.remove(selected_path);

            if(result.failed) {
                show_error_modal = true;
            }

            show_confirm_modal = false;

            {
                // Wake up both pie and menu on change
                std::lock_guard<std::mutex> lock(data->cv_mutex);
                data->menu_path_changed = true;
                data->pie_path_changed = true;
            }

            data->cv.notify_all();
        },
        [&]{
            show_confirm_modal = false;
        }
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

    auto menu_component = BonsaiMenu::menu(&screen, data, &scanner, &selected, default_path, option);
    menu_component = CatchEvent(menu_component, [data, &scanner, &default_path, &show_confirm_modal](Event event) {
        // Mac keyboards have no SUPPR key (their delete key is Backspace, which goes up a folder): D works everywhere
        if (event == Event::Delete || event == Event::Character('d') || event == Event::Character('D')) {
            show_confirm_modal = true;
            return true;
        }

        if(event == Event::Backspace || event == Event::Escape) {
            std::string new_path;
            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);

                if(fs::equivalent(*data->path, default_path)) {
                    return true;
                }

                std::filesystem::path p(*data->path);
                new_path = p.parent_path().string();

                *data->path = new_path;
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
    std::thread menu_thread(BonsaiMenu::worker, &screen, data, &scanner, default_path);


    /* Init pie:
    - No need for a containter this time, as the component isn't interactive
    */
    data->pie_entries = std::make_shared<std::vector<AppData::BonsaiPieEntry>>();

    auto pie_component = BonsaiPie::pie(data, &scanner, default_path);
    std::thread pie_thread(BonsaiPie::worker, &screen, data, &scanner, default_path);

    auto left = Renderer(menu_container, [&] {
        std::lock_guard<std::mutex> lock_m(data->menu_mutex);

        const bool files = scanner.getMetric() == Scanner::Metric::INODES;

        return vbox({
            text(" "),
            hbox({
                text("Explorer") | bold,
                text(files ? " [files]" : " [size]") | color(Color::GrayDark),
                text(" "),
                BonsaiQuotaView::badge(quotas.get(), quotas.ready()),
            }) | center,
            separator(),
            hbox({
                menu_component->Render() | frame | flex,
                text(" ")
            }) | flex,
            separator(),
            text(" U quotas F files D delete Q quit") | color(Color::GrayDark),
        });
    });

    // Right side: the pie chart, or the quotas
    auto right = Renderer(pie_component, [&] {
        if (show_quota)
            return BonsaiQuotaView::render(quotas.get(), quotas.ready());

        return pie_component->Render();
    });

    auto window = ResizableSplitLeft(left, right, &config.SIDEBAR_WIDTH);

    window |= Modal(confirm_modal, &show_confirm_modal);
    window |= Modal(error_modal, &show_error_modal);

    // Quit with q, allow sidebar resize with arrow keys
    auto app = CatchEvent(window, [&](Event event) {
        if (event == Event::Character('q') || event == Event::Character('Q')) {
            screen.Exit();
            return true;
        }

        if ((event == Event::Character('u') || event == Event::Character('U')) && !show_confirm_modal && !show_error_modal) {
            show_quota = !show_quota;
            if (show_quota) quotas.refresh();
            return true;
        }

        if ((event == Event::Character('r') || event == Event::Character('R')) && !show_confirm_modal && !show_error_modal) {
            quotas.refresh();
            return true;
        }

        // Size or file count: the menu and the pie are listed again with the other metric
        if ((event == Event::Character('f') || event == Event::Character('F')) && !show_confirm_modal && !show_error_modal) {
            scanner.setMetric(scanner.getMetric() == Scanner::Metric::BYTES ? Scanner::Metric::INODES : Scanner::Metric::BYTES);

            {
                std::lock_guard<std::mutex> lock(data->cv_mutex);
                data->menu_path_changed = true;
                data->pie_path_changed = true;
            }

            data->cv.notify_all();
            return true;
        }

        if(event == Event::ArrowRight && !show_confirm_modal && !show_error_modal) {
            config.SIDEBAR_WIDTH += 1;
            return true;
        }

        if(event == Event::ArrowLeft && !show_confirm_modal && !show_error_modal) {
            config.SIDEBAR_WIDTH -= 1;
            return true;
        }

        return false;
    });

    // Render UI
    screen.Loop(app);


    // Stop app
    scanner.stop();
    AppData::stop(data);

    // Save config
    config.writeToFile();

    scanner_thread.join();
    menu_thread.join();
    pie_thread.join();

    return 0;
}
