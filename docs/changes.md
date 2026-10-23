# Changes from bonsai

**For ROMEO**

- Scope: only your spaces (home, scratch, `/project/<group>` for each of your groups), and only your files in them.
- Quota view, cleanup suggestions and folder search, plus the `--quota`, `--cleanup` and `--summary` modes.
- A cache of the previous scan, shown while the new one runs (marked `~`). It is merged on save, so scanning one folder keeps the rest.
- `install.sh` builds with cmake from Spack and installs to `~/.local/bin`.
- Quitting waits for one `mmlsquota` call at most when GPFS is slow (it waited for all of them, twice when one timed out).
- `?` marks a folder of yours that the scan doesn't know (made after it). A folder you open again is read again.
- Cleanup suggestions are computed in the background. Old data only counts folders scanned completely, and files already deleted aren't listed.
- For the file quota: folders of many files in the cleanup view, `T` to pack one into a checked `.tar`, and quota cards that open their space sorted by files and say how much to remove.
- `R` scans a folder again, `W` lists what grew since the previous scan, `SPACE` marks entries for one deletion, `S` / `A` order by name or age, `Y` copies a path (OSC 52), `?` lists every key, vim keys, and `--json`.
- Folders that couldn't be read are marked `!` and listed (`!`), instead of being silently counted as smaller.
- A deleted file that changed after the scan started gets its folder scanned again, so the totals stay exact.
- Deleting runs on 16 threads (one folder each, files deleted relative to their folder), the check before it too: 100,000 files in 6 s instead of about 15. A window shows the progress, the files being deleted, and can stop it; packing and scanning again use it too.
- A window whose list was empty (a cleanup filter with nothing in it) took no key at all: ESC and TAB went nowhere.

**Fixes that also apply to bonsai** (reported upstream)

- Colors: FTXUI 7 detects the colors again when its loop starts, and checks `TERM` before `TERM_PROGRAM`. Our choice is applied again after that, and shades are computed from exact RGB values (bonsai's slices turn black past the 6th sibling).
- The chart stays round, using the cell size reported by the terminal.
- ASCII icons, UTF-8 truncation, columns that follow the sidebar width, and labels measured inside their slice and drawn last.
- Hard links are counted once. On scratch, bonsai showed 45 GiB where 34 GiB are on disk.
- The delete dialog shows what it deletes and starts on cancel. Deletion runs in the background, works during the scan, and keeps the totals exact. A folder is checked first, so a permission error never leaves it half deleted (`remove_all` stops at the first one).
- The program quits when the terminal hangs up, and CMake embeds the config (no `xxd -n` needed).
- No more crash when the folder shown is removed by another program (it goes up to the closest one left), on a bad config value, or with an empty color list. Search matches accented, CJK and emoji names, and columns are cut by display width.
- Config: a comment after a value no longer changes it (`CHART_HIGHLIGHT_SUBTREE = true  # ...` was read as false), `SIDEBAR_GROWTH_THRESHOLD_PERCENTAGE` is read, and custom `CHART_COLORS` survive the save. The file is rewritten only when something changed, and in one go: a home over its quota no longer leaves it empty. A bad value only resets its own key.
- The sidebar stays within the terminal, and the width saved is the one you chose, not the one a small terminal forced.
- After going back, changing the metric (`F`) or opening a result, the selection lands on the right folder: it was set before the new list was built.

**Performance** on ROMEO (GPFS, 1.5 million files): 33 s with 16 threads, 26 s with 32 (now the default), 28 s with 64. During a scan, bonsai re-read the folder shown every 100 ms, with two `stat` calls per file. It now reads it once and keeps it 10 s: for a folder of 20,000 files that is 82 times fewer `stat` calls (3.4 million → 41,000 in 14 s). FTXUI is downloaded as a release archive (363 KB) instead of a git clone, which could hang.
