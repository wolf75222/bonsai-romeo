# Bonsai — ROMEO edition (`bonsai-romeo`)

Fork of [orrnithogalum/bonsai](https://github.com/orrnithogalum/bonsai) for users of the
[ROMEO 2025](https://romeo.univ-reims.fr/) cluster (URCA). It only shows **your** data, so you can
clean up your quotas without wading through everyone else's files.

What changes compared to upstream:

- **Only your spaces are scanned**: `/home/$USER`, `/scratch_p/$USER`, and `/project/<group>`
  for every project group you belong to (`id -Gn`). Everything else on GPFS is skipped.
  The explorer starts on the list of your spaces (home, scratch, project ...), not on GPFS's
  folders above them, and shows where you are under its title.
- **Only your files are counted and listed**: in shared `/project/<group>` folders, other
  members' files are invisible, and their folders only appear if they contain files of yours.
- **Numbers that match the quotas**: files with several hard links (conda environments, git,
  spack...) are counted once, like GPFS does, and `F` switches between apparent size, size on
  disk (what the space quota counts) and number of files (what the file quota counts).
- **Instant start**: the previous scan is cached (`~/.cache/bonsai-romeo/db.txt`). Its sizes are
  shown right away, marked `~`, and each folder switches to its new size once it is scanned.
  Scanning one folder only updates that part of the cache.
- **Cleanup suggestions** (key `C`, or `bonsai-romeo --cleanup`): caches and rebuildable folders
  (pip, conda, spack, apptainer caches, `__pycache__`, `node_modules`, build folders, virtual
  envs...), big folders untouched for 6 months, core dumps and your largest files, biggest first,
  with how to clean each one. `TAB` filters by kind, `ENTER` opens the entry in the explorer.
- **Folder search** (key `/`): finds folders by name in all your spaces as you type, biggest
  first. Words are searched in the path, the last one in the folder name (`adc build`: the build
  folders under adc...). `ENTER` opens the folder in the explorer.
- **Quota dashboard** (key `U`, or `bonsai-romeo --quota`): your GPFS quotas with gauges, for
  space *and* number of files, same rules as ROMEO's `/apps/romeotools/myquotas.sh`.
- **Faster scan on GPFS**: up to 32 threads (measured: 33 s with upstream's 16, 26 s with 32, for
  1.5 million files), one pass up the tree per folder, no stat above your spaces.
- **Safer deleting**: only what you own, inside your spaces, never a whole space; the confirmation
  shows what and how much, and starts on "cancel". Big folders are deleted in the background.
- Columns stay aligned in any terminal (ASCII icons, UTF-8 aware truncation), and the pie keeps its
  24-bit colors through SSH and stays round whatever the font.
- `bonsai-romeo --summary [path]`: scan, print size, size on disk and file count of each space.
- `BONSAI_ROOTS=dir1:dir2` overrides the detected spaces, `BONSAI_THREADS=n` the thread count.
- Own config and cache (`~/.config/bonsai-romeo/`, `~/.cache/bonsai-romeo/`), no `xxd` needed to build.

> [!WARNING]
> ROMEO has **no backups**: anything removed with `D` / `SUPPR` is gone for good.

### Install on ROMEO (any user)

On a login node (`ssh romeo`):

```bash
git clone https://github.com/wolf75222/bonsai-romeo.git ~/bonsai-romeo && ~/bonsai-romeo/install.sh
```

`install.sh` takes `cmake` from Spack (`romeo_load_x64cpu_env`), builds with the system `g++`, and
installs the binary in `~/.local/bin/bonsai-romeo` (already in your `PATH` on ROMEO). To update:
`cd ~/bonsai-romeo && git pull && ./install.sh`. Once installed, `rm -rf ~/bonsai-romeo/build`
frees the build files. The binary is x86_64: it runs on the login nodes and the `x64cpu` nodes.

### Run

```bash
bonsai-romeo                       # all your spaces
bonsai-romeo /project/<group>      # one project, only your files in it
bonsai-romeo --quota               # quotas only, no UI
bonsai-romeo --cleanup             # what could be cleaned, no UI
bonsai-romeo --summary             # sizes and file counts, no UI
```

| Key | Action |
|-----|--------|
| `F` | size / size on disk / number of files |
| `/` | search folders (`UP`/`DOWN` select, `ENTER` open, `ESC` close) |
| `C` | cleanup suggestions (`TAB` kind, `ENTER` open, `ESC` close) |
| `U` | quota dashboard / back to the pie chart |
| `R` | refresh the quotas |
| `D` (or `SUPPR`) | delete the selected entry (yours only), after a confirmation |
| `⌫` / `ESC` | go up a folder |
| `ENTER` | open the selected folder |
| `Q` | quit |

On a Mac keyboard `⌫` is Backspace (go up), not delete: use `D` (or `fn` + `⌫`, which sends SUPPR).

### How quotas work on ROMEO

- Each quota has two limits, for space and for files (inodes: every file or directory counts as one):
  - **soft**: can be exceeded during a grace period (a few days); once it expires, writing is blocked
    until you get back below it.
  - **hard**: can never be exceeded.
- `home` and `scratch` have a quota per user. `/project/<group>` has no limit per person: the
  space is counted on the project group, shared by all its members.
- Files you put in a project folder must belong to the project group. Files left with your
  personal group (copied from elsewhere) aren't counted on the project and can make your writes
  fail: the dashboard detects them and shows the `chgrp` commands to fix it.
- The gauge is scaled to the hard limit, `|` marks the soft limit. Quotas count blocks on disk:
  press `F` in the explorer for the size on disk, which matches them (hard links counted once).

### Terminal compatibility

- Icons are plain ASCII (`+` folder, `>` selection, `<` back), so the columns line up in every
  terminal and font. Nerd Font icons can still be set in `~/.config/bonsai-romeo/bonsai.conf`.
- Sizes in the sidebar are short (`282G`, `45.2G`, `911M`) so they always fit their column, and the
  columns follow the sidebar when it is resized with the arrow keys.
- The selected folder turns white in the inner ring and its sub-folders turn paler, in their own
  colors. `CHART_HIGHLIGHT_SUBTREE = true` brings back upstream's look (the whole folder in grays).
- Labels in the pie are as long as their slice allows (up to 16 characters, upstream: 3), and
  are drawn last so no slice covers them; the rings fill the room (2 during the scan, then 4).
- The pie is round on screen: the size of a character cell is read from the terminal (SSH
  forwards it), since cells are rarely exactly twice as tall as wide (Warp: 9 x 20.4 px).
  If your terminal doesn't report it, set `CHART_CELL_RATIO` (cell height / width) in the config.
- Colors: through SSH the program can't ask your terminal whether it supports 24-bit colors
  (ROMEO receives neither `COLORTERM` nor any other variable), and FTXUI then falls back to 256
  colors, which flattens the shades of the pie. bonsai-romeo uses 24-bit colors for known terminals
  (Warp, Ghostty, iTerm2, kitty, WezTerm...) and for SSH sessions in xterm-like terminals.
  If the colors look wrong, use 256 colors: `COLOR_MODE = "256"` in the config, or
  `BONSAI_COLOR=256 bonsai-romeo`.
- If the SSH connection drops, the program quits on its own instead of staying on the login node.

A full scan walks GPFS metadata and takes a minute or two for a few million files; meanwhile the
sizes of the previous scan are shown (marked `~`).

---

# Bonsai

Bonsai is a terminal user interface (TUI) remake of the GNOME Baobab disk usage analyzer.  
It provides a lightweight, fast, and visually structured way to explore disk usage on Linux systems directly in the terminal.

<p align="center">
  <img src="img/app.gif" alt="App preview" width="640">
</p>

## Features

- Navigable sidebar displaying folder sizes, sorted by a scanning algorithm
- Responsive UI
- Pie chart visualization similar to GNOME Baobab:
  - Multiple rings representing nested directories
  - Color-coded sections for files and folders
  - Labels for each item
- Built using **CMake** and **FTXUI** for a modern TUI experience
- Multi-threaded scan, with folder sizes kept in a tree for efficient lookups
- Dynamic pie chart updates when navigating between folders in the sidebar  
- Pie chart highlighting when hovering over folders or files 
- Asynchronous UI rendering
- User customization via configuration file
- Ability to remove files / folders

> [!TIP]
> You can check the scan progress in the top-right corner of your terminal.
> - If a status message is visible → the scanner is still running.
> - If nothing is displayed → the scan has completed.
> - The full pie chart is only displayed when the scanner has completed.

## Planned Features

- Ability to search for folders
- Translations?
- Windows port?

## Dependencies
Before compiling, make sure you have:

- rgb compliant terminal
- cmake and a c compiler
- xxd (usually installed if you have vim)

## Usage

### Keyboard Controls

- `Q` → quit
- `BACKSPACE / ESCAPE` → go up a folder
- `RETURN` → go down a folder
- `UP / DOWN` arrows → navigate menu
- `RIGHT / LEFT` arrows → resize sidebar
- `ESCAPE` → if prompted: exit a modal
- `RETURN` → if prompted: select button in a modal
- `RIGHT / LEFT` arrows → if prompted: navigate modal buttons
- `SUPPR` → delete the selected file / folder

### Mouse Controls
> [!WARNING]
> This app is designed to be keyboard-only, but ftxui does provide some mouse controls

- `SCROLL` → navigate menu
- `LEFT CLICK` → if promted: select button in a modal

## Installation
### Arch Linux (AUR)
You can install Bonsai directly from the AUR:

```bash
# Using an AUR helper like yay
yay -S bonsai

# Using an AUR helper like paru
paru -S bonsai
```

### Other Linux distributions

Build manually from source:
```bash
git clone https://github.com/orrnithogalum/bonsai.git

cd bonsai
mkdir build

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/bonsai
```
