/* PROGRESS VIEW
- A window over the explorer while a long job runs (deleting, packing a folder, scanning one again):
  what it does, a bar, how many entries, how fast, about how long is left, and the entries it is on, live.
- ESC hides it (the job keeps going, a line at the bottom of the sidebar says how far it got, P shows the window again);
  S stops the job. When it ends, the window says how for a moment, then closes.
*/

#pragma once

#include "../../include/core/progress.hpp"
#include "../../include/core/user_scope.hpp"

#include <ftxui/component/component.hpp>
#include <functional>
#include <memory>
#include <string>

class BonsaiProgress {
public:
    // The job shown (UI thread only); no progress when none runs
    struct Job {
        std::string title;                    // "Deleting ~/x"
        std::shared_ptr<Progress> progress;
        bool deleting = false;                // The space freed is shown
        bool finished = false;                // Over: the window says how, then closes by itself
        std::string result;
    };

    static ftxui::Component window(const Job* job, const UserScope* scope,
                                   std::function<void()> on_hide, std::function<void()> on_stop);

    // "41%" for the title line ("" while it can't tell)
    static std::string percent(Progress& progress);
};
