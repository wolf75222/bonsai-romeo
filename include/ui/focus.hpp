/* FOCUS
- A Modal hands keys only to a window that can take the focus. A window made of a list alone can't when
  the list is empty (FTXUI's Menu with no entry isn't focusable): TAB and ESC went nowhere, and the
  window couldn't be closed (a cleanup filter with nothing in it, or nothing that grew).
- keepFocus() makes the window focusable whatever it holds: its keys always reach it.
*/

#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

inline ftxui::Component keepFocus(ftxui::Component window) {
    class Impl : public ftxui::ComponentBase {
    public:
        explicit Impl(ftxui::Component child) { Add(std::move(child)); }
        bool Focusable() const override { return true; }
    };

    return ftxui::Make<Impl>(std::move(window));
}
