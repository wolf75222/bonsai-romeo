#include "../../include/ui/quota_view.hpp"

#include "../../include/utils/format.hpp"

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

// Cell i of the gauge: filled, soft limit marker or empty
int softCell(const Quota::Limit& limit) {
    if (limit.hard == 0 || limit.soft == 0) return -1;
    return std::min(BAR_WIDTH - 1, static_cast<int>(static_cast<double>(limit.soft) / limit.hard * BAR_WIDTH));
}

Element gauge(const Quota::Limit& limit) {
    const int filled = static_cast<int>(limit.ratio() * BAR_WIDTH + 0.5);
    const int marker = softCell(limit);
    const Color fill = stateColor(limit.state());

    Elements cells;
    for (int i = 0; i < BAR_WIDTH; i++) {
        if (i == marker)
            cells.push_back(text("│") | bold | color(i < filled ? Color::White : Color::GrayLight) | bgcolor(i < filled ? fill : Color::GrayDark));
        else if (i < filled)
            cells.push_back(text(" ") | bgcolor(fill));
        else
            cells.push_back(text(" ") | bgcolor(Color::GrayDark));
    }

    return hbox(std::move(cells));
}

Element line(const std::string& label, const Quota::Limit& limit, bool bytes) {
    const Color c = stateColor(limit.state());
    const std::string grace = graceText(limit);

    return hbox({
        text(label) | size(WIDTH, EQUAL, 8),
        gauge(limit),
        text(" " + percent(limit)) | bold | color(c) | size(WIDTH, EQUAL, 6),
        text(amounts(limit, bytes)),
        filler(),
        text(grace.empty() ? left(limit, bytes) : grace) | color(grace.empty() ? Color::GrayLight : c) | bold,
        text(" "),
    });
}

std::string title(const Quota::Row& row) {
    if (row.group) return row.name + " · project space · " + row.path;
    return row.fileset + " · " + row.name + " · " + row.path;
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

    const std::string label = worst >= Quota::State::GRACE ? " ⚠ quota " : " quota ";
    return text(label) | bold | color(Color::Black) | bgcolor(stateColor(worst >= Quota::State::NOTICE ? worst : Quota::State::NOTICE));
}

Element BonsaiQuotaView::render(const Quota::Report& report, bool ready) {
    if (!ready)
        return vbox({text(" "), text("Reading GPFS quotas...") | center}) | flex;

    if (!report.error.empty())
        return vbox({text(" "), text(report.error) | center | color(Color::RGB(255, 70, 70))}) | flex;

    Elements content;
    content.push_back(text(" "));
    content.push_back(hbox({
        text(" Quotas GPFS") | bold,
        filler(),
        text("updated " + ago(report) + " · R refresh · U back to chart ") | color(Color::GrayDark),
    }));
    content.push_back(text(" " + summary(report)) | bold | color(stateColor(report.worst() == Quota::State::NO_LIMIT ? Quota::State::OK : report.worst())));
    content.push_back(text(" "));

    for (const auto& row : report.rows) {
        if (!row.space.limited() && !row.inodes.limited()) {
            // The user quota on projet: counted on the project groups instead
            content.push_back(window(text(" " + title(row) + " "), vbox({
                text(" " + FormatUtils::toReadable(row.space.used, " ") + " · " + FormatUtils::withSeparators(row.inodes.used) + " files") | bold,
                paragraph(" No individual limit here: what you put in a project counts on the project's quota (cards below).") | color(Color::GrayLight),
            })) | color(Color::GrayLight));
            continue;
        }

        const Color c = stateColor(row.state());
        content.push_back(window(text(" " + title(row) + " ") | bold | color(c), vbox({
            line("Space", row.space, true),
            line("Files", row.inodes, false),
        })));
    }

    if (!report.stray_group.empty()) {
        Elements lines;
        for (const auto& l : strayHelp(report)) lines.push_back(paragraph(" " + l));
        content.push_back(window(text(" Files outside of the project quota ") | bold | color(stateColor(Quota::State::GRACE)), vbox(std::move(lines))));
    }

    content.push_back(text(" "));
    content.push_back(hbox({
        text(" ") , text("  ") | bgcolor(stateColor(Quota::State::OK)), text(" fine  "),
        text("  ") | bgcolor(stateColor(Quota::State::NOTICE)), text(" ≥85% of soft  "),
        text("  ") | bgcolor(stateColor(Quota::State::GRACE)), text(" over soft, grace running  "),
        text("  ") | bgcolor(stateColor(Quota::State::HARD)), text(" blocked  "),
        text("│") | bold, text(" soft limit"),
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
        const int marker = softCell(limit);
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
        }
        out << "\n";
    }

    if (!report.stray_group.empty()) {
        out << (color ? ansi(Quota::State::GRACE) : "") << bold << "Files outside of the project quota" << reset << "\n";
        for (const auto& l : strayHelp(report)) out << "  " << l << "\n";
        out << "\n";
    }

    out << "| = soft limit (grace period, then writing blocked) · bar end = hard limit · files = inodes\n";
    return out.str();
}
