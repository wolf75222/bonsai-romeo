#include "../../include/ui/quota_view.hpp"

#include "../../include/utils/format.hpp"
#include "../../include/utils/json.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace {

constexpr int BAR_WIDTH = 32;

Color stateColor(Quota::State s) {
    switch (s) {
        case Quota::State::OK:       return Color::RGB(60, 179, 113);
        case Quota::State::NO_LIMIT: return Color::GrayDark;
        case Quota::State::NOTICE:   return Color::RGB(255, 215, 0);
        case Quota::State::GRACE:    return Color::RGB(255, 140, 0);
        case Quota::State::EXPIRED:
        case Quota::State::HARD:     return Color::RGB(255, 70, 70);
    }
    return Color::Default;
}

const char* ansi(Quota::State s) {
    switch (s) {
        case Quota::State::OK:       return "\033[32m";
        case Quota::State::NO_LIMIT: return "\033[90m";
        case Quota::State::NOTICE:   return "\033[33m";
        case Quota::State::GRACE:    return "\033[38;5;208m";
        case Quota::State::EXPIRED:
        case Quota::State::HARD:     return "\033[31m";
    }
    return "";
}

std::string percent(const Quota::Limit& limit) {
    std::ostringstream ss;
    ss << static_cast<int>(limit.ratio() * 100) << "%";
    return ss.str();
}

std::string value(uint64_t v, bool bytes) {
    return bytes ? FormatUtils::toReadable(v, " ") : FormatUtils::withSeparators(v);
}

// "24.21 GiB / 100.00 GiB  (hard 105.00 GiB)"
std::string amounts(const Quota::Limit& limit, bool bytes) {
    return value(limit.used, bytes) + " / " + value(limit.soft, bytes) + "  (hard " + value(limit.hard, bytes) + ")";
}

std::string left(const Quota::Limit& limit, bool bytes) {
    if (limit.used >= limit.soft) return "0 left";
    return value(limit.soft - limit.used, bytes) + " left";
}

std::string graceText(const Quota::Limit& limit) {
    switch (limit.state()) {
        case Quota::State::GRACE:   return "grace: " + limit.grace + " left";
        case Quota::State::EXPIRED: return "grace expired: writing blocked";
        case Quota::State::HARD:    return "hard limit: writing impossible";
        default:                    return "";
    }
}

// Cell of the gauge where the soft limit is (-1: none)
int softCell(const Quota::Limit& limit, int bar_width) {
    if (limit.hard == 0 || limit.soft == 0) return -1;
    return std::min(bar_width - 1, static_cast<int>(static_cast<double>(limit.soft) / limit.hard * bar_width));
}

Element gauge(const Quota::Limit& limit, int bar_width) {
    const int filled = static_cast<int>(limit.ratio() * bar_width + 0.5);
    const int marker = softCell(limit, bar_width);
    const Color fill = stateColor(limit.state());

    Elements cells;
    for (int i = 0; i < bar_width; i++) {
        if (i == marker)
            cells.push_back(text("|") | bold | color(i < filled ? Color::White : Color::GrayLight) | bgcolor(i < filled ? fill : Color::GrayDark));
        else if (i < filled)
            cells.push_back(text(" ") | bgcolor(fill));
        else
            cells.push_back(text(" ") | bgcolor(Color::GrayDark));
    }

    return hbox(std::move(cells));
}

// Over the soft limit: how much has to go
std::string need(const Quota::Limit& limit, bool bytes) {
    if (limit.soft == 0 || limit.used <= limit.soft)
        return "";
    return "remove " + value(limit.used - limit.soft, bytes) + (bytes ? "" : " files") + " to get under the soft limit";
}

// What is said right of a gauge: what's left, or the grace period
std::string rightText(const Quota::Limit& limit, bool bytes) {
    const std::string grace = graceText(limit);
    return grace.empty() ? left(limit, bytes) : grace;
}

/* Layout of the gauges, the same for every card so the bars and percentages line up
- Wide enough: "Space  [gauge]  23%  24.26 GiB / 100.00 GiB  (hard 105.00 GiB)      75.74 GiB left"
- Narrow terminal: the amounts go on a second line and the gauge shrinks, so nothing is cut
*/
struct Layout {
    bool one_line = true;
    int bar_width = BAR_WIDTH;
};

constexpr int LABEL_WIDTH = 8;
constexpr int PERCENT_WIDTH = 6;

Layout layout(const Quota::Report& report, int width) {
    size_t amount = 0, right = 0;
    for (const auto& row : report.rows) {
        for (int i = 0; i < 2; i++) {
            const Quota::Limit& limit = i == 0 ? row.space : row.inodes;
            if (!limit.limited()) continue;
            amount = std::max(amount, amounts(limit, i == 0).size());
            right = std::max(right, rightText(limit, i == 0).size());
        }
    }

    Layout l;
    const int right_width = static_cast<int>(right) + 1;
    l.one_line = width >= LABEL_WIDTH + BAR_WIDTH + PERCENT_WIDTH + static_cast<int>(amount) + 2 + right_width;
    if (!l.one_line)
        l.bar_width = std::clamp(width - LABEL_WIDTH - PERCENT_WIDTH - right_width - 2, 8, BAR_WIDTH);

    return l;
}

Element line(const std::string& label, const Quota::Limit& limit, bool bytes, const Layout& l) {
    const Color c = stateColor(limit.state());
    const std::string grace = graceText(limit);

    Element first = hbox({
        text(label) | size(WIDTH, EQUAL, LABEL_WIDTH),
        gauge(limit, l.bar_width),
        text(" " + percent(limit)) | bold | color(c) | size(WIDTH, EQUAL, PERCENT_WIDTH),
        text(l.one_line ? amounts(limit, bytes) : ""),
        filler(),
        text(rightText(limit, bytes)) | color(grace.empty() ? Color::GrayLight : c) | bold,
        text(" "),
    });

    Elements lines = {first};
    if (!l.one_line)
        lines.push_back(hbox({text("") | size(WIDTH, EQUAL, LABEL_WIDTH), text(amounts(limit, bytes)) | color(Color::GrayLight)}));

    if (const std::string over = need(limit, bytes); !over.empty())
        lines.push_back(hbox({text("") | size(WIDTH, EQUAL, LABEL_WIDTH), text(over) | bold | color(c)}));

    return vbox(std::move(lines));
}

const char* jsonState(Quota::State s) {
    switch (s) {
        case Quota::State::OK:       return "ok";
        case Quota::State::NO_LIMIT: return "no_limit";
        case Quota::State::NOTICE:   return "notice";
        case Quota::State::GRACE:    return "grace";
        case Quota::State::EXPIRED:  return "expired";
        case Quota::State::HARD:     return "hard";
    }
    return "";
}

std::string jsonLimit(const Quota::Limit& limit) {
    return "{\"used\": " + std::to_string(limit.used) + ", \"soft\": " + std::to_string(limit.soft) + ", \"hard\": "
         + std::to_string(limit.hard) + ", \"grace\": " + Json::str(limit.grace) + ", \"state\": \"" + jsonState(limit.state()) + "\"}";
}

// "12.3 GiB (7%)": a part of a total
std::string share(uint64_t part, uint64_t total, bool bytes) {
    std::string out = bytes ? FormatUtils::toReadable(part, " ") : FormatUtils::withSeparators(part) + " files";
    if (total > 0)
        out += " (" + std::to_string(static_cast<int>(std::min(100.0, 100.0 * static_cast<double>(part) / static_cast<double>(total)))) + "%)";
    return out;
}

std::string title(const Quota::Row& row) {
    if (row.group) return row.name + " - project space - " + row.path;
    return row.fileset + " - " + row.name + " - " + row.path;
}

std::string ago(const Quota::Report& report) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - report.fetched_at).count();
    if (seconds < 60) return std::to_string(seconds) + " s ago";
    return std::to_string(seconds / 60) + " min ago";
}

std::string summary(const Quota::Report& report) {
    const int alerts = report.alerts();
    if (alerts > 0) return std::to_string(alerts) + " quota(s) need your attention";
    if (report.worst() == Quota::State::NOTICE) return "Some quotas are getting close to their soft limit";
    return "All your quotas are within limits";
}

std::vector<std::string> strayHelp(const Quota::Report& report) {
    std::vector<std::string> lines;
    std::string dirs;
    for (const auto& g : report.project_groups) dirs += " /project/" + g;

    lines.push_back(FormatUtils::withSeparators(report.stray_inodes) + " file(s) (" + FormatUtils::toReadable(report.stray_bytes, " ")
                    + ") in /project belong to your personal group '" + report.stray_group + "' instead of the project.");
    lines.push_back("They aren't counted on the project and can make your writes fail. To fix:");
    lines.push_back("  find" + dirs + " -group " + report.stray_group + " -user $USER | head");
    for (const auto& g : report.project_groups)
        lines.push_back("  find /project/" + g + " -group " + report.stray_group + " -user $USER -exec chgrp " + g + " {} +");
    lines.push_back("  find" + dirs + " -user $USER -type d ! -perm -2000 -exec chmod g+s {} +");
    return lines;
}

}

Element BonsaiQuotaView::badge(const Quota::Report& report, bool ready) {
    if (!ready || !report.error.empty()) return text("");

    const Quota::State worst = report.worst();
    if (worst < Quota::State::NOTICE && report.stray_group.empty()) return text("");

    const std::string label = worst >= Quota::State::GRACE ? " ! quota " : " quota ";
    return text(label) | bold | color(Color::Black) | bgcolor(stateColor(worst >= Quota::State::NOTICE ? worst : Quota::State::NOTICE));
}

std::vector<BonsaiQuotaView::Target> BonsaiQuotaView::targets(const Quota::Report& report, const UserScope& scope) {
    std::vector<Target> out;

    for (size_t i = 0; i < report.rows.size() && out.size() < 9; i++) {
        const Quota::Row& row = report.rows[i];
        if (!row.space.limited() && !row.inodes.limited())
            continue;

        // The space of this quota, by the path the user knows it by (/home/<user> is shown as ~)
        for (const auto& root : scope.roots) {
            const std::string shown = scope.display(root);
            if (shown == row.path || (!row.group && row.fileset == "home" && shown == "~")) {
                out.push_back(Target{i, root, row.inodes.ratio() > row.space.ratio()});
                break;
            }
        }
    }

    return out;
}

Element BonsaiQuotaView::render(const Quota::Report& report, bool ready, int width, const UserScope& scope,
                                const std::function<Scanner::Totals(const std::string&)>& mine) {
    // Inside a card: minus its two borders
    const Layout gauges = layout(report, std::max(width - 2, 20));

    if (!ready)
        return vbox({text(" "), text("Reading GPFS quotas...") | center}) | flex;

    if (!report.error.empty())
        return vbox({text(" "), text(report.error) | center | color(Color::RGB(255, 70, 70))}) | flex;

    Elements content;
    content.push_back(text(" "));
    content.push_back(hbox({
        text(" Quotas GPFS") | bold,
        filler(),
        text("updated " + ago(report) + " - R refresh - 1-9 open - U back to chart ") | color(Color::GrayDark),
    }));
    content.push_back(text(" " + summary(report)) | bold | color(stateColor(report.worst() == Quota::State::NO_LIMIT ? Quota::State::OK : report.worst())));
    content.push_back(text(" "));

    // Files that can make writes fail: first, so they never fall below the screen
    if (!report.stray_group.empty()) {
        Elements lines;
        for (const auto& l : strayHelp(report)) lines.push_back(paragraph(" " + l));
        content.push_back(window(text(" Files outside of the project quota ") | bold | color(stateColor(Quota::State::GRACE)), vbox(std::move(lines))));
    }

    const std::vector<Target> numbered = targets(report, scope);

    for (size_t i = 0; i < report.rows.size(); i++) {
        const Quota::Row& row = report.rows[i];
        if (!row.space.limited() && !row.inodes.limited()) {
            // The user quota on projet: counted on the project groups instead
            content.push_back(window(text(" " + title(row) + " "), vbox({
                text(" " + FormatUtils::toReadable(row.space.used, " ") + " - " + FormatUtils::withSeparators(row.inodes.used) + " files") | bold,
                paragraph(" No individual limit here: what you put in a project counts on the project's quota (cards below).") | color(Color::GrayLight),
            })) | color(Color::GrayLight));
            continue;
        }

        const Color c = stateColor(row.state());
        const auto target = std::find_if(numbered.begin(), numbered.end(), [i](const Target& t) { return t.row == i; });
        const std::string key = target != numbered.end() ? "[" + std::to_string(target - numbered.begin() + 1) + "] " : "";

        Elements lines = {
            line("Space", row.space, true, gauges),
            line("Files", row.inodes, false, gauges),
        };

        // In a project: what of it is the user's (from the scan; ~ while it runs)
        if (row.group && target != numbered.end()) {
            const Scanner::Totals t = mine(target->root);
            if (t.known) {
                const std::string about = t.final ? "" : "~";
                lines.push_back(hbox({text("") | size(WIDTH, EQUAL, LABEL_WIDTH),
                                      text("yours: " + about + share(t.disk, row.space.used, true) + ", " + about + share(t.inodes, row.inodes.used, false))
                                      | color(Color::GrayLight)}));
            }
        }

        content.push_back(window(text(" " + key + title(row) + " ") | bold | color(c), vbox(std::move(lines))));
    }


    content.push_back(text(" "));
    content.push_back(hbox({
        text(" ") , text("  ") | bgcolor(stateColor(Quota::State::OK)), text(" fine  "),
        text("  ") | bgcolor(stateColor(Quota::State::NOTICE)), text(" >=85% of soft  "),
        text("  ") | bgcolor(stateColor(Quota::State::GRACE)), text(" over soft, grace running  "),
        text("  ") | bgcolor(stateColor(Quota::State::HARD)), text(" blocked  "),
        text("|") | bold, text(" soft limit"),
    }) | color(Color::GrayLight));
    content.push_back(paragraph(" Soft limit: can be exceeded during the grace period, then writing is blocked. "
                                "Hard limit: never exceeded. Files = inodes: every file or directory counts as one. "
                                "Press F in the explorer to see where your files are.") | color(Color::GrayDark));

    return vbox(std::move(content)) | vscroll_indicator | yframe | flex;
}

std::string BonsaiQuotaView::toText(const Quota::Report& report, bool color) {
    std::ostringstream out;
    const char* reset = color ? "\033[0m" : "";
    const char* bold = color ? "\033[1m" : "";

    if (!report.error.empty()) {
        out << report.error << "\n";
        return out.str();
    }

    auto bar = [&](const Quota::Limit& limit) {
        const int filled = static_cast<int>(limit.ratio() * BAR_WIDTH + 0.5);
        const int marker = softCell(limit, BAR_WIDTH);
        std::string s = color ? ansi(limit.state()) : "";
        s += "[";
        for (int i = 0; i < BAR_WIDTH; i++)
            s += i == marker ? "|" : (i < filled ? "#" : ".");
        s += "]";
        s += reset;
        return s;
    };

    out << bold << "GPFS quotas" << reset << " - " << summary(report) << "\n\n";

    for (const auto& row : report.rows) {
        out << (color ? ansi(row.state()) : "") << bold << title(row) << reset << "\n";

        if (!row.space.limited() && !row.inodes.limited()) {
            out << "  " << FormatUtils::toReadable(row.space.used, " ") << ", " << FormatUtils::withSeparators(row.inodes.used)
                << " files - no individual limit, counted on the project quotas\n\n";
            continue;
        }

        for (int i = 0; i < 2; i++) {
            const Quota::Limit& limit = i == 0 ? row.space : row.inodes;
            const bool bytes = i == 0;
            const std::string grace = graceText(limit);

            out << "  " << std::left << std::setw(6) << (bytes ? "Space" : "Files") << bar(limit) << " "
                << std::right << std::setw(4) << percent(limit) << "  " << amounts(limit, bytes) << "  "
                << (color && !grace.empty() ? ansi(limit.state()) : "") << (grace.empty() ? left(limit, bytes) : grace) << reset << "\n";

            if (const std::string over = need(limit, bytes); !over.empty())
                out << "        " << (color ? ansi(limit.state()) : "") << over << reset << "\n";
        }
        out << "\n";
    }

    if (!report.stray_group.empty()) {
        out << (color ? ansi(Quota::State::GRACE) : "") << bold << "Files outside of the project quota" << reset << "\n";
        for (const auto& l : strayHelp(report)) out << "  " << l << "\n";
        out << "\n";
    }

    out << "| = soft limit (grace period, then writing blocked) - bar end = hard limit - files = inodes\n";
    return out.str();
}

std::string BonsaiQuotaView::toJson(const Quota::Report& report) {
    std::ostringstream out;
    out << "{\"rows\": [";
    for (size_t i = 0; i < report.rows.size(); i++) {
        const Quota::Row& row = report.rows[i];
        out << (i ? ",\n  " : "\n  ") << "{\"name\": " << Json::str(row.name) << ", \"fileset\": " << Json::str(row.fileset)
            << ", \"path\": " << Json::str(row.path) << ", \"group\": " << (row.group ? "true" : "false")
            << ", \"state\": \"" << jsonState(row.state()) << "\", \"space\": " << jsonLimit(row.space)
            << ", \"files\": " << jsonLimit(row.inodes) << "}";
    }
    out << "\n],\n\"stray\": ";
    if (report.stray_group.empty())
        out << "null";
    else
        out << "{\"group\": " << Json::str(report.stray_group) << ", \"bytes\": " << report.stray_bytes << ", \"files\": " << report.stray_inodes << "}";
    out << ",\n\"error\": " << (report.error.empty() ? "null" : Json::str(report.error)) << "}\n";
    return out.str();
}
