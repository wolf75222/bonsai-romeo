/* MODAL CONFIRM
- Modal component to confirm a deletion (or packing a folder into one file, key T).
- Shows what will be done (`text`), and starts on "cancel" (`focused` = 0): ENTER also opens
  folders, so a D followed by an ENTER typed out of habit must not delete anything.
- Two buttons (exit = cancel, action = the button's label) that will each execute their respective function on click
*/

#pragma once

#include <ftxui/component/component.hpp>
#include <string>

using namespace ftxui;

class BonsaiModalConfirm {
public:
    // What the window says; changed before it is shown
    struct Text {
        std::string title = "Delete this for good?";
        std::string target;   // One line per '\n': what, then how much
        std::string warning = "ROMEO has no backups: this can't be undone.";
        std::string button = "delete";
    };

    static Component confirm(std::function<void()> action, std::function<void()> exit, const Text* text, int* focused);
};
