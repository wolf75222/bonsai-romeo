# Terminals

bonsai-romeo is meant to be used over SSH, from any terminal.

- **Colors.** Over SSH a program can't ask the terminal whether it supports 24-bit colors: `COLORTERM` isn't forwarded, and ROMEO accepts no environment variable from the client. bonsai-romeo uses 24-bit colors for known terminals (Ghostty, kitty, WezTerm, Alacritty, iTerm2, Warp, VS Code) and for SSH sessions in an xterm-like terminal. In Apple's Terminal it uses 256 colors. If the colors look wrong, set `BONSAI_COLOR=256` or `COLOR_MODE = "256"`.
- **Round chart.** Character cells are rarely exactly twice as tall as wide (Warp's are 9 × 20.4 px). bonsai-romeo reads the cell size from the terminal, which SSH forwards. If your terminal doesn't report it, set `CHART_CELL_RATIO` to the height / width of a cell (often 2.0 to 2.3).
- **Alignment.** Icons are plain ASCII, names are cut by characters (not bytes), and sizes fit their column, so rows line up whatever the font.
- **Mac keyboards.** `⌫` is Backspace, which goes up a folder. To delete, use `D` or `fn` + `⌫`.
- **Dropped connection.** If SSH drops, bonsai-romeo quits instead of staying on the login node.
