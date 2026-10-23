# Changes from bonsai

**For ROMEO**

- Scope: only your spaces (home, scratch, `/project/<group>` for each of your groups), and only your files in them.
- Quota view, cleanup suggestions and folder search, plus the `--quota`, `--cleanup` and `--summary` modes.
- A cache of the previous scan, shown while the new one runs (marked `~`). It is merged on save, so scanning one folder keeps the rest.
- `install.sh` builds with cmake from Spack and installs to `~/.local/bin`.

**Fixes that also apply to bonsai** (reported upstream)

- Colors: FTXUI 7 detects the colors again when its loop starts, and checks `TERM` before `TERM_PROGRAM`. Our choice is applied again after that, and shades are computed from exact RGB values (bonsai's slices turn black past the 6th sibling).
- The chart stays round, using the cell size reported by the terminal.
- ASCII icons, UTF-8 truncation, columns that follow the sidebar width, and labels measured inside their slice and drawn last.
- Hard links are counted once. On scratch, bonsai showed 45 GiB where 34 GiB are on disk.
- The delete dialog shows what it deletes and starts on cancel. Deletion runs in the background, works during the scan, and keeps the totals exact. A folder is checked first, so a permission error never leaves it half deleted (`remove_all` stops at the first one).
- The program quits when the terminal hangs up, and CMake embeds the config (no `xxd -n` needed).

**Performance** on ROMEO (GPFS, 1.5 million files): 33 s with 16 threads, 26 s with 32 (now the default), 28 s with 64.
