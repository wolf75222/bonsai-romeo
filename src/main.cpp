#include "../include/ui/modal_confirm.hpp"
#include "../include/ui/modal_error.hpp"
#include "../include/config/config.hpp"
#include "../include/core/app_data.hpp"
#include "../include/core/archive.hpp"
#include "../include/core/scanner.hpp"
#include "../include/ui/piechart.hpp"
#include "../include/ui/menu.hpp"
#include "../include/ui/quota_view.hpp"
#include "../include/ui/cleanup_view.hpp"
#include "../include/ui/errors_view.hpp"
#include "../include/ui/growth_view.hpp"
#include "../include/ui/help_view.hpp"
#include "../include/ui/progress_view.hpp"
#include "../include/ui/search_view.hpp"
#include "../include/utils/format.hpp"
#include "../include/utils/json.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <filesystem>
#include <ftxui/screen/terminal.hpp>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <csignal>
#include <cstdio>
#include <iostream>
#include <sstream>
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

/* Clipboard (key Y)
- OSC 52 asks the terminal itself to copy: it works through SSH, from the login node to the user's computer
- Most terminals do it (iTerm2 asks first, tmux needs set-clipboard on); the rest ignore it silently
*/
static void copyToClipboard(const std::string& value) {
    static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;

    for (size_t i = 0; i < value.size(); i += 3) {
        const uint32_t a = static_cast<unsigned char>(value[i]);
        const uint32_t b = i + 1 < value.size() ? static_cast<unsigned char>(value[i + 1]) : 0;
        const uint32_t c = i + 2 < value.size() ? static_cast<unsigned char>(value[i + 2]) : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;

        encoded += digits[(triple >> 18) & 63];
        encoded += digits[(triple >> 12) & 63];
        encoded += i + 1 < value.size() ? digits[(triple >> 6) & 63] : '=';
        encoded += i + 2 < value.size() ? digits[triple & 63] : '=';
    }

    std::cout << "\033]52;c;" << encoded << "\a" << std::flush;
}

int main(int argc, char* argv[]) {
    using namespace ftxui;

    bool summary = false;
    bool quota_only = false;
    bool cleanup_only = false;
    bool json = false;
    std::vector<std::string> args;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];

        if (arg == "--summary") {
            summary = true;
        } else if (arg == "--quota") {
            quota_only = true;
        } else if (arg == "--cleanup") {
            cleanup_only = true;
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: bonsai-romeo [--summary | --quota | --cleanup] [--json] [path]\n"
                         "  Shows only your own data: home, scratch and your /project/<group> folders.\n"
                         "  --summary   scan, print size, disk usage and file count of each of your spaces, and exit\n"
                         "  --quota     print your GPFS quotas (space and files) with gauges, and exit\n"
                         "  --cleanup   scan, print what could be cleaned (caches, builds, old folders and files, many files, large files), and exit\n"
                         "  --json      with one of the above: the same as JSON, for scripts\n"
                         "  BONSAI_ROOTS=dir1:dir2   override the detected spaces\n"
                         "  BONSAI_THREADS=n         scan threads (default: automatic, up to 32)\n"
                         "  BONSAI_COLOR=truecolor|256|16   force the color mode (default: auto)\n"
                         "Keys (? in the program lists them all):\n"
                         "  ENTER/l open, Backspace/ESC/h up, j/k g/G move, F size / disk / files, S order, A age column,\n"
                         "  R scan the folder again, SPACE mark, X clear marks, D delete, T pack a folder into one .tar,\n"
                         "  Y copy the path, / search, C cleanup, U quotas (1-9 open a space), W what grew, ! unreadable folders, Q quit\n"
                         "Sizes marked ~ come from the previous scan (or are still growing), ? aren't scanned, ! hold an unreadable folder.\n";
            return 0;
        } else {
            args.push_back(arg);
        }
    }

    if (json && !summary && !quota_only && !cleanup_only) {
        std::cerr << "--json goes with --summary, --quota or --cleanup" << std::endl;
        return 1;
    }

    if (quota_only) {
        const Quota::Report report = Quota::fetch();
        std::cout << (json ? BonsaiQuotaView::toJson(report) : BonsaiQuotaView::toText(report, isatty(STDOUT_FILENO)));
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
        const auto items = scanner.cleanup();
        std::cout << (json ? BonsaiCleanup::toJson(items, scope) : BonsaiCleanup::toText(items, scope, 40));
        if (!json)
            std::cerr << BonsaiErrors::toText(scanner.readErrors(10), scanner.readErrorCount(), scope);
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

        const std::vector<Scanner::ReadError> errors = scanner.readErrors(100);
        const uint32_t error_count = scanner.readErrorCount();

        Scanner::Totals total;
        std::ostringstream spaces;
        for (size_t i = 0; i < shown.size(); i++) {
            const std::string& path = shown[i];
            const Scanner::Totals t = scanner.totals(path);
            total.bytes += t.bytes;
            total.disk += t.disk;
            total.inodes += t.inodes;

            if (json) {
                spaces << (i ? ",\n  " : "\n  ") << "{\"path\": " << Json::str(path) << ", \"shown\": " << Json::str(scope.display(path))
                       << ", \"bytes\": " << t.bytes << ", \"disk\": " << t.disk << ", \"files\": " << t.inodes
                       << ", \"read_errors\": " << (t.error ? "true" : "false") << "}";
            } else {
                std::printf("%10s %10s on disk %10s files   %s%s\n", FormatUtils::toReadableShort(t.bytes).c_str(), FormatUtils::toReadableShort(t.disk).c_str(),
                            FormatUtils::toCount(t.inodes).c_str(), scope.display(path).c_str(), t.error ? "  (!)" : "");
            }
        }

        if (json) {
            std::cout << "{\"spaces\": [" << spaces.str() << "\n],\n\"total\": {\"bytes\": " << total.bytes << ", \"disk\": " << total.disk
                      << ", \"files\": " << total.inodes << "},\n\"directories\": " << scanner.directories()
                      << ",\n\"read_error_count\": " << error_count << ",\n\"read_errors\": [";
            for (size_t i = 0; i < errors.size(); i++) {
                std::cout << (i ? ",\n  " : "\n  ") << "{\"path\": " << Json::str(errors[i].path) << ", \"shown\": " << Json::str(scope.display(errors[i].path))
                          << ", \"error\": " << Json::str(std::strerror(errors[i].error)) << "}";
            }
            std::cout << "\n]}\n";
        } else {
            std::printf("%10s %10s on disk %10s files   total (%u directories)\n", FormatUtils::toReadableShort(total.bytes).c_str(), FormatUtils::toReadableShort(total.disk).c_str(),
                        FormatUtils::toCount(total.inodes).c_str(), scanner.directories());
            std::cout << BonsaiErrors::toText(errors, error_count, scope);
        }

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
    bool show_help = false;
    bool show_growth = false;
    bool show_errors = false;

    Scanner scanner = Scanner(default_path, db_path, scope);
    std::thread scanner_thread([&scanner, &screen]() {
        scanner.scan();
        scanner.snapshot();
        screen.PostEvent(Event::Custom);
    });

    // Init shared menu & pie data
    int selected = 0;
    auto data = std::make_shared<AppData::BonsaiData>();

    data->menu_entries = std::make_shared<std::vector<AppData::BonsaiMenuEntry>>();
    data->menu_labels = std::make_shared<std::vector<std::string>>();

    data->path = std::make_shared<std::string>(default_path);
    data->selected = std::make_shared<int>(0);

    // Wakes the menu and the pie up: the folder shown, its order or its sizes changed
    auto wake = [&data] {
        {
            std::lock_guard<std::mutex> lock(data->cv_mutex);
            data->menu_path_changed = true;
            data->pie_path_changed = true;
        }
        data->cv.notify_all();
    };

    // A message under the title for a few seconds (UI thread only)
    std::string notice;
    auto notice_until = std::chrono::steady_clock::now();
    auto say = [&](const std::string& message) {
        notice = message;
        notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    };

    /* Init confirm & error modals
    - Allow quitting modal with button
    - Allow quitting modal with escape key
    */
    bool show_confirm_modal = false;
    bool show_error_modal = false;
    std::string error_title;
    std::string error_message;

    /* Background jobs: deleting, packing a folder into one file, scanning a folder again
    - On their own thread, one at a time: GPFS can take minutes, the explorer stays usable
    - The progress window shows how far they got, live (see BonsaiProgress); ESC hides it, S stops the job
    - When one ends, a notice says how it went, or the error window does
    - Quitting waits for a deletion to finish; packing and scanning are stopped (nothing is lost)
    */
    struct JobResult {
        bool failed = false;
        std::string title;                 // Of the error window, when failed
        std::string message;               // The notice, or the error
        std::vector<std::string> deleted;  // Paths gone: their marks go too
    };
    enum class Job { DELETE, PACK, RESCAN };

    std::thread job_thread;
    std::thread job_ticker;
    std::atomic<bool> job_running{false};  // Until the UI took its result
    std::atomic<bool> job_working{false};  // Until its thread is done (the UI may be gone by then)
    std::atomic<bool> quitting{false};
    Job job_kind = Job::DELETE;            // UI thread only
    std::string job_label;                 // UI thread only: "deleting ~/x"
    std::atomic<pid_t> tar_child{0};

    // The progress window (key P shows it again once hidden)
    BonsaiProgress::Job progress_job;      // UI thread only
    bool show_progress = false;
    bool progress_hidden = false;

    // Set once the marks summary exists (below): what is deleted leaves the marks
    std::function<void()> marks_changed;

    // Closes the progress window a moment after its job ended (a thread: nothing else redraws then)
    std::thread progress_closer;
    std::atomic<bool> closer_running{false};

    // UI thread: what a job did
    auto finishJob = [&](const JobResult& r) {
        job_running = false;
        progress_hidden = false;

        // Still shown: the window says how it ended, for a moment (an error has its own window)
        if (show_progress && !r.failed) {
            progress_job.finished = true;
            progress_job.result = r.message;

            // One at a time; the UI never waits for it (when one still sleeps, ESC closes this window)
            if (!closer_running) {
                if (progress_closer.joinable())
                    progress_closer.join();
                closer_running = true;
                progress_closer = std::thread([&, progress = progress_job.progress] {
                    for (int i = 0; i < 25 && !quitting; i++)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    screen.Post([&, progress] {
                        if (progress_job.finished && progress_job.progress == progress)
                            show_progress = false;
                    });
                    screen.PostEvent(Event::Custom);
                    closer_running = false;
                });
            }
        } else {
            show_progress = false;
        }

        for (const auto& gone : r.deleted) {
            for (auto it = data->marked.begin(); it != data->marked.end();)
                it = UserScope::isUnder(*it, gone) ? data->marked.erase(it) : std::next(it);
        }
        if (marks_changed)
            marks_changed();

        data->listing_version++;

        if (r.failed) {
            error_title = r.title;
            error_message = r.message;
            show_error_modal = true;
        } else if (!r.message.empty()) {
            say(r.message);
        }

        wake();
    };

    // "Deleting ~/x" for the window, from "deleting ~/x"
    auto titled = [](std::string label) {
        if (!label.empty())
            label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        return label;
    };

    auto startJob = [&](const std::string& label, Job kind, std::function<JobResult(Progress&)> work) {
        if (job_running) {
            show_progress = true;
            progress_hidden = false;
            return;
        }
        if (job_thread.joinable())
            job_thread.join();
        if (job_ticker.joinable())
            job_ticker.join();

        auto progress = std::make_shared<Progress>();
        job_running = true;
        job_working = true;
        job_kind = kind;
        job_label = label;
        progress_job = BonsaiProgress::Job{titled(label), progress, kind != Job::RESCAN, false, ""};
        progress_hidden = false;

        job_thread = std::thread([&, work, progress] {
            const JobResult r = work(*progress);
            job_working = false;
            screen.Post([&, r] { finishJob(r); });
            screen.PostEvent(Event::Custom);
        });

        // Redrawn a few times a second while it runs; the window opens unless it is over at once (one file)
        job_ticker = std::thread([&, progress] {
            const auto start = std::chrono::steady_clock::now();
            bool opened = false;
            while (job_working) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                if (!opened && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(400)) {
                    opened = true;
                    screen.Post([&, progress] {
                        if (job_running && progress_job.progress == progress && !progress_hidden)
                            show_progress = true;
                    });
                }
                screen.PostEvent(Event::Custom);
            }
        });
    };

    // From a job's thread: what the title and the window say
    auto relabel = [&](const std::string& label) {
        screen.Post([&, label] {
            job_label = label;
            progress_job.title = titled(label);
        });
        screen.PostEvent(Event::Custom);
    };

    /* Deleting (D)
    - On several threads, with its progress (see Scanner::remove()); stopping leaves the rest as it is
    - What is inside a folder deleted just before is gone with it
    - A folder stopped midway, or a deleted file that changed since the scan, may not match what the scan
      counted: it is scanned again, each folder once, so the totals stay exact
    */
    auto removeAll = [&](const std::vector<std::string>& paths, Progress& progress) {
        JobResult r;
        std::vector<std::string> failures;
        std::vector<std::string> rescans;
        std::string remark;
        bool stopped = false;

        for (size_t i = 0; i < paths.size() && !stopped; i++) {
            const std::string& path = paths[i];
            if (std::any_of(r.deleted.begin(), r.deleted.end(), [&path](const std::string& d) { return UserScope::isUnder(path, d); })) {
                r.deleted.push_back(path);
                continue;
            }

            if (paths.size() > 1)
                relabel("deleting " + scope.display(path) + " (" + std::to_string(i + 1) + " of " + std::to_string(paths.size()) + ")");

            const Scanner::ScannerRemoveResult removed = scanner.remove(path, &progress);
            stopped = progress.stop.load();
            if (!removed.rescan.empty())
                rescans.push_back(removed.rescan);

            if (removed.failed) {
                failures.push_back((paths.size() > 1 ? scope.display(path) + ": " : "") + removed.reason);
                continue;
            }

            if (stopped) {
                remark = removed.reason.empty() ? "stopped" : removed.reason;
                if (!removed.rescan.empty())
                    continue;
            } else if (!removed.reason.empty()) {
                remark = removed.reason;
            }
            r.deleted.push_back(path);
        }

        // Exact totals, even after a stop (S again skips it)
        progress.stop = false;
        std::sort(rescans.begin(), rescans.end());
        rescans.erase(std::unique(rescans.begin(), rescans.end()), rescans.end());
        for (size_t i = 0; i < rescans.size() && !quitting && !progress.stop.load(); i++) {
            if (i > 0 && UserScope::isUnder(rescans[i], rescans[i - 1]))
                continue;
            relabel("scanning " + scope.display(rescans[i]) + " again for exact totals");
            scanner.rescan(rescans[i], &progress);
        }

        if (!failures.empty()) {
            r.failed = true;
            r.title = paths.size() == 1 ? "Deleting " + FormatUtils::trimMiddle(scope.display(paths.front()), 60) + " failed"
                                        : std::to_string(failures.size()) + " of " + std::to_string(paths.size()) + " entries couldn't be deleted";
            for (size_t i = 0; i < failures.size() && i < 8; i++)
                r.message += (i ? "\n" : "") + failures[i];
            if (failures.size() > 8)
                r.message += "\n... and " + std::to_string(failures.size() - 8) + " more";
            return r;
        }

        const std::string took = " in " + FormatUtils::toDuration(std::chrono::duration<double>(std::chrono::steady_clock::now() - progress.started).count());
        if (stopped)
            r.message = remark;
        else
            r.message = (paths.size() == 1 ? "deleted " + scope.display(paths.front()) : "deleted " + std::to_string(paths.size()) + " entries") + took
                      + (remark.empty() ? "" : " (" + remark + ")");
        return r;
    };

    // Packing a folder into one .tar (T), then deleting the folder once the archive is checked
    auto packFolder = [&](const std::string& path, Progress& progress) {
        JobResult r;
        const Archive::Result packed = Archive::pack(path, progress, tar_child);

        if (!packed.ok) {
            if (progress.stop.load()) {
                r.message = "stopped: nothing was packed nor deleted";
                return r;
            }
            r.failed = true;
            r.title = "Packing " + FormatUtils::trimMiddle(scope.display(path), 60) + " failed";
            r.message = "Nothing was deleted. " + packed.error;
            return r;
        }

        scanner.addFile(packed.archive);

        relabel("deleting " + scope.display(path) + " (packed)");
        const Scanner::ScannerRemoveResult removed = scanner.remove(path, &progress);
        if (removed.failed || progress.stop.load()) {
            if (!removed.rescan.empty()) {
                progress.stop = false;
                scanner.rescan(removed.rescan, &progress);
            }
            r.failed = removed.failed;
            r.title = "Packed, but the folder is still there";
            r.message = scope.display(packed.archive) + " is complete and checked, but " + scope.display(path)
                      + (removed.failed ? " couldn't be deleted: " + removed.reason : " wasn't deleted: " + removed.reason);
            return r;
        }

        r.deleted.push_back(path);
        r.message = "packed " + scope.display(path) + " into " + fs::path(packed.archive).filename().string() + ": "
                  + FormatUtils::withSeparators(packed.entries) + " entries, now 1 file";
        return r;
    };

    // Scanning folders again (R)
    auto rescanAll = [&](const std::vector<std::string>& paths, Progress& progress) {
        JobResult r;
        for (const auto& path : paths) {
            if (quitting || progress.stop.load())
                break;
            relabel("scanning " + scope.display(path) + " again");
            const Scanner::RescanResult done = scanner.rescan(path, &progress);
            if (!done.message.empty())
                r.message = done.failed || done.message.rfind("stopped", 0) == 0 ? done.message : "scanned again: " + done.message;
        }
        if (paths.size() > 1 && r.message.rfind("scanned again", 0) == 0)
            r.message = "scanned your " + std::to_string(paths.size()) + " spaces again";
        return r;
    };

    // What D and T ask before doing anything
    BonsaiModalConfirm::Text confirm_text;
    enum class Confirm { DELETE, PACK };
    Confirm confirm_kind = Confirm::DELETE;
    std::vector<std::string> confirm_paths;
    int confirm_focus = 0;   // 0: cancel, 1: delete / pack

    auto confirm_modal = BonsaiModalConfirm::confirm(
        [&]() {
            show_confirm_modal = false;
            if (confirm_paths.empty())
                return;

            if (confirm_kind == Confirm::PACK) {
                const std::string path = confirm_paths.front();
                startJob("packing " + scope.display(path), Job::PACK, [&, path](Progress& progress) { return packFolder(path, progress); });
            } else {
                const std::vector<std::string> paths = confirm_paths;
                const std::string label = paths.size() == 1 ? "deleting " + scope.display(paths.front()) : "deleting " + std::to_string(paths.size()) + " entries";
                startJob(label, Job::DELETE, [&, paths](Progress& progress) { return removeAll(paths, progress); });
            }
        },
        [&]{
            show_confirm_modal = false;
        },
        &confirm_text,
        &confirm_focus
    );

    confirm_modal = CatchEvent(confirm_modal, [&show_confirm_modal](Event event) {
        if(event == Event::Escape) {
            show_confirm_modal = false;
            return true;
        }

        return false;
    });

    auto error_modal = BonsaiModalError::error(&error_title, &error_message,
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

    // "1.20 GiB on disk, 12.3k files" of a folder or a file (~: not final yet)
    auto amountOf = [&](const std::string& path, bool is_dir, uint64_t& disk, uint64_t& files) {
        if (is_dir) {
            const Scanner::Totals totals = scanner.totals(path);
            disk = totals.disk;
            files = totals.inodes;
            return !totals.final;
        }

        // fs::file_size() gives uintmax_t(-1) on error, shown as 17179869184 GiB
        struct stat st;
        disk = lstat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_blocks) * 512 : 0;
        files = 1;
        return false;
    };

    // What the marked entries hold, for the line under the list: worked out when the marks change (it stats them)
    std::string marks_line;
    auto refreshMarks = [&] {
        marks_line.clear();
        if (data->marked.empty())
            return;

        uint64_t disk = 0, files = 0;
        std::string previous;
        for (const auto& path : data->marked) {
            // Inside a marked folder: already in its totals
            if (!previous.empty() && UserScope::isUnder(path, previous))
                continue;
            previous = path;

            std::error_code ec;
            uint64_t d = 0, f = 0;
            amountOf(path, fs::is_directory(fs::symlink_status(path, ec)), d, f);
            disk += d;
            files += f;
        }

        marks_line = " " + std::to_string(data->marked.size()) + " marked: " + FormatUtils::toReadable(disk, " ") + ", "
                   + FormatUtils::toCount(files) + " files  (D deletes, X clears)";
    };
    marks_changed = refreshMarks;

    // The selected entry of the explorer (empty path for ".."), from the Menu's own index
    struct Selection {
        std::string path;
        std::string label;
        bool is_dir = false;
    };
    auto selection = [&]() {
        Selection s;
        std::lock_guard<std::mutex> lock(data->menu_mutex);
        // The shared copy of the index may lag one list behind
        if (selected >= 0 && selected < static_cast<int>(data->menu_entries->size())) {
            const auto& entry = (*data->menu_entries)[selected];
            s.path = entry.path;
            s.label = entry.label;
            s.is_dir = entry.is_dir;
        }
        return s;
    };

    // Asks the menu to select `label` in the list it makes next for `folder` (after F, S...)
    auto keepSelection = [&]() {
        std::lock_guard<std::mutex> lock(data->menu_mutex);
        if (selected >= 0 && selected < static_cast<int>(data->menu_entries->size())) {
            data->select_path = *data->path;
            data->select_label = (*data->menu_entries)[selected].label;
            data->select_generation = ++data->list_generation;
        }
    };

    /* Init menu:
    - Use a container to keep focus through the entire render.
      If a menu component is embeded in a slew of elements, it becomes static
    - Allow going back in tree with escape or backspace keys
    */
    MenuOption option;

    auto menu_inner = BonsaiMenu::menu(&screen, data, &scanner, &selected, &config.SIDEBAR_WIDTH, default_path, option);
    auto menu_component = CatchEvent(menu_inner, [&](Event event) {
        // Mac keyboards have no SUPPR key (their delete key is Backspace, which goes up a folder): D works everywhere
        if (event == Event::Delete || event == Event::Character('d') || event == Event::Character('D')) {
            if (job_running) {
                show_progress = true;
                progress_hidden = false;
                return true;
            }

            // The marked entries, or the selected one; inside a marked folder is gone with it
            std::vector<std::string> paths;
            if (!data->marked.empty()) {
                for (const auto& path : data->marked) {
                    if (paths.empty() || !UserScope::isUnder(path, paths.back()))
                        paths.push_back(path);
                }
            } else {
                const Selection s = selection();
                // ".." has no path
                if (s.path.empty())
                    return true;
                paths.push_back(s.path);
            }

            // A whole space (all of home, all of scratch) is never one key away
            for (const auto& path : paths) {
                if (scope.isRoot(path)) {
                    error_title = "Not deleted";
                    error_message = scope.display(path) + " is one of your spaces: open it and delete what is inside instead.";
                    show_error_modal = true;
                    return true;
                }
            }

            uint64_t disk = 0, files = 0;
            bool about = false;
            for (const auto& path : paths) {
                std::error_code ec;
                uint64_t d = 0, f = 0;
                about |= amountOf(path, fs::is_directory(fs::symlink_status(path, ec)), d, f);
                disk += d;
                files += f;
            }

            // "~": not final yet (from the previous scan, or still being scanned)
            const std::string tilde = about ? "~" : "";
            const std::string amount = "(" + tilde + FormatUtils::toReadable(disk, " ") + " on disk, " + tilde + FormatUtils::toCount(files, " ") + " files)";

            // Paths cut in their middle to the screen (their end, the name, stays), the size on its own line
            const size_t room = static_cast<size_t>(std::max(20, std::min(ftxui::Terminal::Size().dimx, 100) - 8));
            std::string target;
            for (size_t i = 0; i < paths.size() && i < 6; i++)
                target += FormatUtils::trimMiddle(scope.display(paths[i]), room) + "\n";
            if (paths.size() > 6)
                target += "... and " + std::to_string(paths.size() - 6) + " more\n";

            confirm_kind = Confirm::DELETE;
            confirm_paths = paths;
            confirm_text.title = paths.size() == 1 ? "Delete this for good?" : "Delete these " + std::to_string(paths.size()) + " entries for good?";
            confirm_text.target = target + amount;
            confirm_text.warning = "ROMEO has no backups: this can't be undone.";
            confirm_text.button = "delete";
            confirm_focus = 0;
            show_confirm_modal = true;
            return true;
        }

        // Thousands of files become one: the file quota counts one file per entry
        if (event == Event::Character('t') || event == Event::Character('T')) {
            if (job_running) {
                show_progress = true;
                progress_hidden = false;
                return true;
            }

            const Selection s = selection();
            if (s.path.empty() || !s.is_dir) {
                say("T packs a folder: select one");
                return true;
            }
            if (scope.isRoot(s.path) || !scope.owns(s.path) || scope.relation(s.path) != UserScope::Relation::INSIDE) {
                say("only a folder of yours inside a space can be packed");
                return true;
            }

            uint64_t disk = 0, files = 0;
            const std::string tilde = amountOf(s.path, true, disk, files) ? "~" : "";
            const std::string archive = fs::path(Archive::archiveName(s.path)).filename().string();
            const size_t room = static_cast<size_t>(std::max(20, std::min(ftxui::Terminal::Size().dimx, 100) - 8));

            confirm_kind = Confirm::PACK;
            confirm_paths = {s.path};
            confirm_text.title = "Pack this folder into one file?";
            confirm_text.target = FormatUtils::trimMiddle(scope.display(s.path), room) + "\ninto " + archive + " next to it\n("
                                + tilde + FormatUtils::withSeparators(files) + " files, " + tilde + FormatUtils::toReadable(disk, " ") + " on disk)";
            confirm_text.warning = "It is deleted once the archive is checked: needs " + tilde + FormatUtils::toReadable(disk, " ") + " more space until then.";
            confirm_text.button = "pack";
            confirm_focus = 0;
            show_confirm_modal = true;
            return true;
        }

        // Marks: SPACE on an entry, then D deletes them all at once
        if (event == Event::Character(' ')) {
            const Selection s = selection();
            if (s.path.empty())
                return true;
            if (scope.isRoot(s.path)) {
                say("a whole space can't be marked: open it");
                return true;
            }

            if (!data->marked.erase(s.path))
                data->marked.insert(s.path);
            refreshMarks();

            // On to the next entry, as in most file managers
            menu_inner->OnEvent(Event::ArrowDown);
            return true;
        }

        // Every entry of the folder shown; when they all are marked already, none
        if (event == Event::Character('*')) {
            std::vector<std::string> entries;
            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);
                for (const auto& entry : *data->menu_entries) {
                    if (!entry.path.empty() && !scope.isRoot(entry.path))
                        entries.push_back(entry.path);
                }
            }
            if (entries.empty())
                return true;

            const bool all = std::all_of(entries.begin(), entries.end(), [&](const std::string& p) { return data->marked.count(p) > 0; });
            for (const auto& path : entries) {
                if (all) data->marked.erase(path);
                else data->marked.insert(path);
            }
            refreshMarks();
            if (all)
                say("unmarked " + std::to_string(entries.size()) + " entries");
            return true;
        }

        // The path, for a shell: ~ written out, so it works between quotes too
        if (event == Event::Character('y') || event == Event::Character('Y')) {
            const Selection s = selection();
            std::string path = s.path.empty() ? *data->path : s.path;
            if (scanner.isSpacesView(path)) {
                say("select something to copy its path");
                return true;
            }

            std::string shown = scope.display(path);
            if (shown == "~" || shown.rfind("~/", 0) == 0)
                shown = home.string() + shown.substr(1);

            copyToClipboard(shown);
            say("copied " + shown);
            return true;
        }

        // Vim keys: j and k (and PgUp, PgDn, Home, End) are the Menu's own
        if (event == Event::Character('l'))
            return menu_inner->OnEvent(Event::Return);
        if (event == Event::Character('g'))
            return menu_inner->OnEvent(Event::Home);
        if (event == Event::Character('G'))
            return menu_inner->OnEvent(Event::End);

        if(event == Event::Backspace || event == Event::Escape || event == Event::Character('h')) {
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

            wake();
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
        const AppData::Sort sort = data->sort.load();

        /* Header: the title, with what F and S show when it isn't the usual, then where the explorer is
        - At the top (your spaces) the list says it all: no path
        - What runs and what happened go at the bottom, the keys are in ? (one line of the main ones)
        */
        const std::string here = scanner.isSpacesView(*data->path) ? "" : scope.display(*data->path);
        const size_t width = static_cast<size_t>(std::max(config.SIDEBAR_WIDTH - 2, 8));

        std::string shown;
        auto tag = [&shown](const char* what) { shown += (shown.empty() ? "" : ", ") + std::string(what); };
        if (metric == Scanner::Metric::DISK) tag("disk");
        if (metric == Scanner::Metric::INODES) tag("files");
        if (sort == AppData::Sort::NAME) tag("by name");
        if (sort == AppData::Sort::AGE) tag("oldest first");

        // A quota needing attention turns the U of the keys line orange: the one place it shows
        const bool quota_alert = !BonsaiQuotaView::badgeText(quotas.get(), quotas.ready()).empty();

        Elements bottom;

        // A job whose window is hidden (P shows it), or what just happened, for a few seconds
        if (job_running && !show_progress) {
            std::string busy = job_label;
            if (progress_job.progress) {
                if (const std::string done = BonsaiProgress::percent(*progress_job.progress); !done.empty())
                    busy += "  " + done;
            }
            bottom.push_back(text(" " + FormatUtils::trimMiddle(busy + "  (P)", width)) | color(Color::GrayLight));
        } else if (!notice.empty() && std::chrono::steady_clock::now() < notice_until) {
            bottom.push_back(text(" " + FormatUtils::trimMiddle(notice, width)) | color(Color::RGB(60, 179, 113)));
        }

        // Marked entries (SPACE): what D would delete
        if (!marks_line.empty())
            bottom.push_back(text(FormatUtils::trimSuffix(marks_line, width + 1, "...")) | color(Color::RGB(255, 215, 0)));

        // Folders that couldn't be read: the sizes above them may be low
        if (const uint32_t errors = scanner.readErrorCount(); errors > 0) {
            bottom.push_back(text(" ! " + std::to_string(errors) + (errors == 1 ? " unreadable folder (!)" : " unreadable folders (!)"))
                             | color(Color::RGB(255, 140, 0)));
        }

        bottom.push_back(separator());
        bottom.push_back(hbox({
            text(" ? keys  / find  C clean  ") | color(Color::GrayDark),
            text("U quota") | color(quota_alert ? Color::RGB(255, 140, 0) : Color::GrayDark),
        }));

        Elements rows = {
            text(" "),
            hbox({
                text("Explorer") | bold,
                text(shown.empty() ? "" : " [" + shown + "]") | color(Color::GrayDark),
            }) | center,
            text(" " + FormatUtils::trimMiddle(here, width)) | color(Color::GrayLight),
            separator(),
            hbox({
                menu_component->Render() | frame | flex,
                text(" ")
            }) | flex,
        };
        rows.insert(rows.end(), bottom.begin(), bottom.end());

        return vbox(std::move(rows));
    });

    // Right side: the pie chart, or the quotas
    auto right = Renderer(pie_component, [&] {
        if (show_quota)
            return BonsaiQuotaView::render(quotas.get(), quotas.ready(), ftxui::Terminal::Size().dimx - config.SIDEBAR_WIDTH - 1,
                                           scanner.getScope(), [&scanner](const std::string& root) { return scanner.totals(root); });

        return pie_component->Render();
    });

    auto window = ResizableSplitLeft(left, right, &config.SIDEBAR_WIDTH);

    // Opens `folder` itself in the explorer, at its first entry (never above where the explorer starts)
    auto open_folder = [&](const std::string& folder) {
        if (!UserScope::isUnder(folder, default_path.native()))
            return false;

        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            *data->path = folder;
            data->select_path = folder;
            data->select_label.clear();
            data->select_generation = ++data->list_generation;
        }
        wake();
        return true;
    };

    /* Opening a path in the explorer (from the cleanup, search, growth or errors window)
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

        wake();
        show_quota = false;
    };

    /* Cleanup suggestions (key C)
    - Looked for on their own thread (the walk covers the whole tree, ~700k folders): the window opens at once
    - Asked again while looking (C, ESC, C, or O for another age): looked for again with the latest age once done
    */
    auto cleanup_state = std::make_shared<BonsaiCleanup::State>();
    cleanup_state->old_days = std::max(1, config.CLEANUP_OLD_DAYS);
    std::thread cleanup_thread;
    std::atomic<bool> cleanup_running{false};

    std::function<void()> lookForCleanup = [&] {
        cleanup_state->computing = true;
        cleanup_state->all.clear();
        BonsaiCleanup::apply(*cleanup_state);

        if (cleanup_running)
            return;
        if (cleanup_thread.joinable())
            cleanup_thread.join();

        cleanup_running = true;
        cleanup_thread = std::thread([&, days = cleanup_state->old_days] {
            auto items = std::make_shared<std::vector<Scanner::CleanupItem>>(scanner.cleanup(days));
            const bool partial = !scanner.isDone();
            cleanup_running = false;

            screen.Post([&, items, partial, days] {
                if (days != cleanup_state->old_days)
                    lookForCleanup();
                else
                    BonsaiCleanup::set(*cleanup_state, std::move(*items), partial);
            });
            screen.PostEvent(Event::Custom);
        });
    };

    auto cleanup_component = BonsaiCleanup::window(cleanup_state, &scanner.getScope(),
        [&](const Scanner::CleanupItem& item) {
            open_in_explorer(item.path);
            show_cleanup = false;
        },
        [&] { show_cleanup = false; },
        [&] { lookForCleanup(); });

    // What grew since the previous scan (key W), worked out on its own thread too
    auto growth_state = std::make_shared<BonsaiGrowth::State>();
    std::thread growth_thread;
    std::atomic<bool> growth_running{false};
    auto growth_component = BonsaiGrowth::window(growth_state, &scanner,
        [&](const std::string& path) {
            open_in_explorer(path);
            show_growth = false;
        },
        [&] { show_growth = false; });

    // Folders that couldn't be read (key !)
    auto errors_state = std::make_shared<BonsaiErrors::State>();
    auto errors_component = BonsaiErrors::window(errors_state, &scanner.getScope(),
        [&](const std::string& path) {
            open_in_explorer(path);
            show_errors = false;
        },
        [&] { show_errors = false; });

    auto help_component = BonsaiHelp::window([&] { show_help = false; });

    auto progress_component = BonsaiProgress::window(&progress_job, &scanner.getScope(),
        [&] {
            show_progress = false;
            progress_hidden = !progress_job.finished;
        },
        [&] {
            if (!progress_job.progress)
                return;
            progress_job.progress->stop = true;
            if (const pid_t child = tar_child.load(); child > 0)
                kill(child, SIGTERM);
        });

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
    window |= Modal(growth_component, &show_growth);
    window |= Modal(errors_component, &show_errors);
    window |= Modal(help_component, &show_help);
    window |= Modal(progress_component, &show_progress);
    window |= Modal(search_component, &show_search);

    auto modal_open = [&] {
        return show_confirm_modal || show_error_modal || show_cleanup || show_search || show_help || show_growth || show_errors || show_progress;
    };

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

        if (event == Event::Character('?') && !modal_open()) {
            show_help = true;
            return true;
        }

        // What runs in the background, again
        if ((event == Event::Character('p') || event == Event::Character('P')) && !modal_open()) {
            if (job_running) {
                show_progress = true;
                progress_hidden = false;
            }
            return true;
        }

        if ((event == Event::Character('u') || event == Event::Character('U')) && !modal_open()) {
            show_quota = !show_quota;
            if (show_quota) quotas.refresh();
            return true;
        }

        // In the quota view: a card's number opens its space, showing what fills it most
        if (show_quota && !modal_open() && event.is_character() && event.character().size() == 1
            && event.character()[0] >= '1' && event.character()[0] <= '9') {
            const auto targets = BonsaiQuotaView::targets(quotas.get(), scanner.getScope());
            const size_t index = static_cast<size_t>(event.character()[0] - '1');
            if (index >= targets.size())
                return true;

            if (!open_folder(targets[index].root)) {
                say("that space is above where the explorer started");
                return true;
            }

            scanner.setMetric(targets[index].files ? Scanner::Metric::INODES : Scanner::Metric::DISK);
            data->sort = AppData::Sort::SIZE;
            show_quota = false;
            say(targets[index].files ? "biggest by files: the file quota" : "biggest on disk: the space quota");
            return true;
        }

        // R: the quotas again in the quota view, otherwise the folder shown scanned again
        if ((event == Event::Character('r') || event == Event::Character('R')) && !modal_open()) {
            if (show_quota) {
                quotas.refresh();
                return true;
            }

            std::string shown;
            {
                std::lock_guard<std::mutex> lock(data->menu_mutex);
                shown = *data->path;
            }

            std::vector<std::string> paths;
            if (scanner.isSpacesView(shown))
                paths = scope.roots;
            else if (scope.relation(shown) == UserScope::Relation::INSIDE)
                paths = {shown};

            if (paths.empty()) {
                say("open a space to scan it again");
                return true;
            }

            startJob(paths.size() > 1 ? "scanning your spaces again" : "scanning " + scope.display(shown) + " again", Job::RESCAN,
                     [&, paths](Progress& progress) { return rescanAll(paths, progress); });
            return true;
        }

        if ((event == Event::Character('c') || event == Event::Character('C')) && !modal_open()) {
            show_cleanup = true;
            lookForCleanup();
            return true;
        }

        if ((event == Event::Character('w') || event == Event::Character('W')) && !modal_open()) {
            show_growth = true;

            if (growth_running)
                return true;
            if (growth_thread.joinable())
                growth_thread.join();

            growth_state->computing = true;
            growth_running = true;
            growth_thread = std::thread([&] {
                auto items = std::make_shared<std::vector<Scanner::GrowthItem>>(scanner.growth(200));
                const bool partial = !scanner.isDone();
                const bool files = scanner.getMetric() == Scanner::Metric::INODES;
                const int64_t since = scanner.previousScan();

                screen.Post([&, items, partial, files, since] {
                    BonsaiGrowth::set(*growth_state, std::move(*items), partial, files, since);
                });
                screen.PostEvent(Event::Custom);
                growth_running = false;
            });
            return true;
        }

        if (event == Event::Character('!') && !modal_open()) {
            const uint32_t count = scanner.readErrorCount();
            if (count == 0) {
                say("every folder could be read");
                return true;
            }

            BonsaiErrors::set(*errors_state, scanner.readErrors(500), count);
            show_errors = true;
            return true;
        }

        if ((event == Event::Character('x') || event == Event::Character('X')) && !modal_open()) {
            if (!data->marked.empty()) {
                data->marked.clear();
                refreshMarks();
                say("marks cleared");
            }
            return true;
        }

        // Size, size on disk, file count: the menu and the pie are listed again with the next metric
        if ((event == Event::Character('f') || event == Event::Character('F')) && !modal_open()) {
            const Scanner::Metric metric = scanner.getMetric();
            scanner.setMetric(metric == Scanner::Metric::BYTES ? Scanner::Metric::DISK
                            : metric == Scanner::Metric::DISK ? Scanner::Metric::INODES : Scanner::Metric::BYTES);

            // The list is sorted again: the same entry stays selected (looked up in the new list)
            keepSelection();
            wake();
            return true;
        }

        // Order: size (of what F shows), name, oldest change first (the age column comes with it)
        if ((event == Event::Character('s') || event == Event::Character('S')) && !modal_open()) {
            const AppData::Sort sort = data->sort.load();
            const AppData::Sort next = sort == AppData::Sort::SIZE ? AppData::Sort::NAME
                                     : sort == AppData::Sort::NAME ? AppData::Sort::AGE : AppData::Sort::SIZE;
            data->sort = next;
            if (next == AppData::Sort::AGE)
                data->show_age = true;

            keepSelection();
            wake();
            return true;
        }

        if ((event == Event::Character('a') || event == Event::Character('A')) && !modal_open()) {
            data->show_age = !data->show_age;
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
    if (growth_thread.joinable())
        growth_thread.join();

    /* What still runs in the background
    - Packing or scanning again: stopped (the folder stays as it was, the .part is removed)
    - Deleting: waited for, with its progress in the terminal (S in the window stops it before quitting);
      the scans again it would do after it are skipped
    */
    quitting = true;
    if (job_working && job_kind != Job::DELETE && progress_job.progress) {
        progress_job.progress->stop = true;
        if (const pid_t child = tar_child.load(); child > 0)
            kill(child, SIGTERM);
    }
    if (!job_working || job_kind != Job::DELETE)
        scanner.stop();

    if (job_working && job_kind == Job::DELETE && progress_job.progress) {
        std::cerr << "Waiting for " << job_label << " to finish (Ctrl+C leaves the rest as it is)..." << std::endl;
        while (job_working) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const std::string done = BonsaiProgress::percent(*progress_job.progress);
            if (job_working && !done.empty())
                std::cerr << "  " << done << " (" << FormatUtils::withSeparators(progress_job.progress->done.load()) << " entries)" << std::endl;
        }
    }

    if (job_thread.joinable())
        job_thread.join();
    if (job_ticker.joinable())
        job_ticker.join();
    if (progress_closer.joinable())
        progress_closer.join();


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
