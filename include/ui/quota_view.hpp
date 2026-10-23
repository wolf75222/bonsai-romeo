/* QUOTA VIEW
- Shows the GPFS quotas of the user (see Quota) in place of the pie chart (key U).
- One card per quota, with a gauge for the space and one for the inodes:
    - The gauge is scaled to the hard limit, `|` marks the soft limit.
    - green: fine, yellow: close to the soft limit, orange: grace period running, red: writing blocked.
- toText() gives the same report for the terminal (--quota).
*/

#pragma once

#include "../../include/core/quota.hpp"

#include <ftxui/dom/elements.hpp>
#include <string>

using namespace ftxui;

class BonsaiQuotaView {
public:
    // width: columns available for the view (the right side of the window)
    static Element render(const Quota::Report& report, bool ready, int width);
    static std::string toText(const Quota::Report& report, bool ansi_colors);

    // Short status for the sidebar header ("" when everything is fine)
    static Element badge(const Quota::Report& report, bool ready);
};
