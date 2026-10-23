/* THEME
- The look shared with RSV, the Slurm viewer for ROMEO (github.com/wolf75222/RSV): the same colors, panels
  with their title in the frame, and keys drawn the same way (the key in the accent color, what it does dimmed)
- Colors are RGB: FTXUI turns them into the closest 256 or 16 color when the terminal has fewer
- Text stays in the terminal's own foreground, so it reads on light and dark themes; the selection is inverted
*/

#pragma once

#include <ftxui/dom/elements.hpp>

#include <string>
#include <utility>
#include <vector>

namespace ui {

namespace theme {
inline ftxui::Color green() { return ftxui::Color::RGB(90, 185, 105); }
inline ftxui::Color yellow() { return ftxui::Color::RGB(222, 165, 50); }
inline ftxui::Color red() { return ftxui::Color::RGB(226, 86, 86); }
inline ftxui::Color blue() { return ftxui::Color::RGB(80, 145, 235); }
inline ftxui::Color magenta() { return ftxui::Color::RGB(190, 110, 220); }
inline ftxui::Color cyan() { return ftxui::Color::RGB(60, 175, 195); }
inline ftxui::Color gray() { return ftxui::Color::RGB(128, 128, 128); }
inline ftxui::Color accent() { return ftxui::Color::RGB(100, 160, 240); }
}

// A key and what it does: "d delete"
inline ftxui::Element keyHint(const std::string& key, const std::string& label, ftxui::Color key_color = theme::accent()) {
    using namespace ftxui;
    return hbox({text(key) | bold | color(key_color), text(" " + label + "  ") | dim});
}

inline ftxui::Element keyHints(const std::vector<std::pair<std::string, std::string>>& keys) {
    ftxui::Elements out;
    for (const auto& [key, label] : keys)
        out.push_back(keyHint(key, label));
    return ftxui::hbox(std::move(out));
}

/* A framed box with its title in the top line of the frame, and something on the right of that line
- Only the frame and the title take the color (FTXUI's window() has none, color() would tint the content)
- The title line is put in a vbox: alone in the dbox, it would get the whole height, its bold and color with it
*/
inline ftxui::Element panel(const std::string& title, ftxui::Element content, ftxui::Color frame = theme::accent(),
                            ftxui::Element right = nullptr) {
    using namespace ftxui;
    Element framed = std::move(content) | borderStyled(ROUNDED, frame);
    Elements top = {text("") | size(WIDTH, EQUAL, 1), text(" " + title + " ") | bold | color(frame), filler()};
    if (right)
        top.push_back(hbox({text(" "), right, text(" ")}) | color(frame));
    top.push_back(text("") | size(WIDTH, EQUAL, 1));
    return dbox({framed, vbox({hbox(std::move(top)), filler()})}) | clear_under;
}

// A window over the others: a blank margin keeps its frame from merging with the frames behind it (┴, ╭╭)
inline ftxui::Element overlay(ftxui::Element window) {
    return std::move(window) | ftxui::borderEmpty;
}

}
