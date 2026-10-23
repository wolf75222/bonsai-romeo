/* HELP VIEW
- A window over the explorer (key ?) listing every key. ESC or ? closes it.
*/

#pragma once

#include <ftxui/component/component.hpp>
#include <functional>

class BonsaiHelp {
public:
    static ftxui::Component window(std::function<void()> on_close);
};
