/* ERRORS VIEW
- A window over the explorer (key !) listing the folders that couldn't be read completely during the scan
  (see DirTree::markError): what they hold isn't counted, so the sizes above them may be too low.
- ENTER opens the folder's parent in the explorer with it selected; ESC or ! closes the window.
*/

#pragma once

#include "../../include/core/scanner.hpp"

#include <ftxui/component/component.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class BonsaiErrors {
public:
    struct State {
        std::vector<Scanner::ReadError> errors;
        std::vector<std::string> labels;   // One per error: the menu needs a list of strings
        uint32_t total = 0;                // Even beyond the ones listed
        int selected = 0;
    };

    // UI thread only
    static void set(State& state, std::vector<Scanner::ReadError> errors, uint32_t total);

    // The errors as text, for --summary
    static std::string toText(const std::vector<Scanner::ReadError>& errors, uint32_t total, const UserScope& scope);

    static ftxui::Component window(std::shared_ptr<State> state, const UserScope* scope,
                                   std::function<void(const std::string&)> on_open,
                                   std::function<void()> on_close);
};
