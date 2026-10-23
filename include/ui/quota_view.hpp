/* QUOTA VIEW
- Shows the GPFS quotas of the user (see Quota) in place of the pie chart (key U).
- One card per quota, with a gauge for the space and one for the inodes:
    - The gauge is scaled to the hard limit, `|` marks the soft limit.
    - green: fine, yellow: close to the soft limit, orange: grace period running, red: writing blocked.
    - Over a soft limit, the card says how much to remove to get back under it.
    - A project card says how much of it is the user's (from the scan).
    - Cards of the user's spaces are numbered: 1 to 9 opens that space in the explorer, showing files
      when the file quota is the fuller one.
- toText() gives the same report for the terminal (--quota), toJson() for scripts (--quota --json).
*/

#pragma once

#include "../../include/core/quota.hpp"
#include "../../include/core/scanner.hpp"

#include <ftxui/dom/elements.hpp>
#include <functional>
#include <string>
#include <vector>

using namespace ftxui;

class BonsaiQuotaView {
public:
    // A numbered card: the space it opens in the explorer
    struct Target {
        size_t row;        // In report.rows
        std::string root;  // Real path of the space
        bool files;        // The file quota is the fuller one
    };
    static std::vector<Target> targets(const Quota::Report& report, const UserScope& scope);

    /* render(report, ready, width, scope, mine)
    - width: columns available for the view (the right side of the window)
    - mine: the scan's totals of a space, for the user's share of a project
    */
    static Element render(const Quota::Report& report, bool ready, int width, const UserScope& scope,
                          const std::function<Scanner::Totals(const std::string&)>& mine);
    static std::string toText(const Quota::Report& report, bool ansi_colors);
    static std::string toJson(const Quota::Report& report);

    // Short status for the sidebar header ("" when everything is fine), and its text
    static Element badge(const Quota::Report& report, bool ready);
    static std::string badgeText(const Quota::Report& report, bool ready);
};
