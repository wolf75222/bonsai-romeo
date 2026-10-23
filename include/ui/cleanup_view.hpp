/* CLEANUP VIEW
- A window over the explorer (key C) listing what could be cleaned, biggest first (see Scanner::cleanup()):
  caches and rebuildable folders, folders untouched for months, core dumps, the largest files.
- ENTER opens the selected entry in the explorer, where D deletes it. ESC or C closes the window.
- TAB shows one kind at a time: all, caches and rebuildable folders, old data, folders of many files, files.
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
        bool computing = false;            // The suggestions are being looked for (on their own thread)
        int64_t now = 0;
    };

    // Fills `state` with suggestions computed elsewhere (Scanner::cleanup() walks the whole tree): UI thread only
    static void set(State& state, std::vector<Scanner::CleanupItem> items, bool partial);

    // Keeps the items of the current filter
    static void apply(State& state);

    // The suggestions as text, for --cleanup (and as JSON, for --cleanup --json)
    static std::string toText(const std::vector<Scanner::CleanupItem>& items, const UserScope& scope, size_t max_items);
    static std::string toJson(const std::vector<Scanner::CleanupItem>& items, const UserScope& scope);

    static ftxui::Component window(std::shared_ptr<State> state, const UserScope* scope,
                                   std::function<void(const Scanner::CleanupItem&)> on_open,
                                   std::function<void()> on_close);
};
