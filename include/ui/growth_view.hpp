/* GROWTH VIEW
- A window over the explorer (key W) listing the folders that grew the most since the previous scan
  (see Scanner::growth()), for what the explorer shows: size, size on disk or files (F).
- Each growth is listed where it happened, not on every folder above it.
- ENTER opens the folder in the explorer; ESC or W closes the window.
*/

#pragma once

#include "../../include/core/scanner.hpp"

#include <ftxui/component/component.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class BonsaiGrowth {
public:
    struct State {
        std::vector<Scanner::GrowthItem> items;
        std::vector<std::string> labels;   // One per item: the menu needs a list of strings
        int selected = 0;
        bool computing = false;            // Being worked out (on its own thread)
        bool partial = false;              // The scan is still running
        bool files = false;                // Growth in files (F), otherwise in bytes
        int64_t since = 0;                 // When the previous scan was saved (0: none)
        int64_t now = 0;
    };

    // UI thread only
    static void set(State& state, std::vector<Scanner::GrowthItem> items, bool partial, bool files, int64_t since);

    static ftxui::Component window(std::shared_ptr<State> state, Scanner* scanner,
                                   std::function<void(const std::string&)> on_open,
                                   std::function<void()> on_close);
};
