/* CLEANUP VIEW
- A window over the explorer (key C) listing what could be cleaned, biggest first (see Scanner::cleanup()):
  caches and rebuildable folders, folders untouched for months, core dumps, the largest files.
- ENTER opens the selected entry in the explorer, where D deletes it. ESC or C closes the window.
- TAB shows one kind at a time: all, caches and rebuildable folders, old data, files.
- Paths are shown as the user knows them: ~ for home, /scratch_p/<user>, /project/<group>.
*/

#pragma once

#include "../../include/core/scanner.hpp"

#include <ftxui/component/component.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class BonsaiCleanup {
public:
    struct State {
        std::vector<Scanner::CleanupItem> all;
        std::vector<Scanner::CleanupItem> items;   // The ones the filter keeps
        std::vector<std::string> labels;   // One per item: the menu needs a list of strings
        int filter = 0;                    // See FILTERS in cleanup_view.cpp
        int selected = 0;
        bool partial = false;              // The scan is still running
        int64_t now = 0;
    };

    // Fills `state` from the scanner, when the window opens
    static void fill(State& state, Scanner& scanner);

    // Keeps the items of the current filter
    static void apply(State& state);

    // The suggestions as text, for --cleanup
    static std::string toText(const std::vector<Scanner::CleanupItem>& items, const UserScope& scope, size_t max_items);

    static ftxui::Component window(std::shared_ptr<State> state, const UserScope* scope,
                                   std::function<void(const Scanner::CleanupItem&)> on_open,
                                   std::function<void()> on_close);
};
