/* MODAL CONFIRM
- Modal component to confirm a deletion.
- Shows what will be deleted (`target`), and starts on "cancel" (`focused` = 0): ENTER also opens
  folders, so a D followed by an ENTER typed out of habit must not delete anything.
- Two buttons (exit = cancel, action = delete) that will each execute their respective function on click
*/

#pragma once

#include <ftxui/component/component.hpp>
#include <string>

using namespace ftxui;

class BonsaiModalConfirm {
public:
    static Component confirm(std::function<void()> action, std::function<void()> exit, const std::string* target, int* focused);
};
