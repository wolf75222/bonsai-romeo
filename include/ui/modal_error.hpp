/* MODAL ERROR
- Simple modal component to display error information: a title, then the message (one paragraph per line).
- Contains one exit button to close the modal
*/

#pragma once

#include <ftxui/component/component.hpp>
#include <string>

using namespace ftxui;

class BonsaiModalError {
public:
    static Component error(const std::string* title, const std::string* message, std::function<void()> exit);
};
