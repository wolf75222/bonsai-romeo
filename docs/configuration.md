# Configuration

The config file is `~/.config/bonsai-romeo/bonsai.conf`, created on the first run. The main settings:

| Key | Default | |
|---|---|---|
| `SCANNER_THREADS` | `0` | scan threads; `0` picks them automatically (up to 32) |
| `COLOR_MODE` | `"auto"` | `"truecolor"`, `"256"` or `"16"` to force the colors |
| `CHART_CELL_RATIO` | `0` | height / width of a character cell, keeps the chart round; `0` asks the terminal |
| `CHART_HIGHLIGHT_SUBTREE` | `false` | `true`: the selected folder turns gray, as in bonsai |
| `CHART_MAX_GENERATIONS` | `4` | number of rings in the chart |
| `CLEANUP_OLD_DAYS` | `180` | days without changes after which a big folder counts as old data |
| `ENABLE_FOLDER_SIZE_PERCENTAGES` | `true` | show the change since the previous scan |
| `SIDEBAR_WIDTH` | `40` | also changed with `←` / `→`, and saved |
| `SIDEBAR_*_ICON` | ASCII | Nerd Font icons work too, if your terminal draws them one column wide |

Environment variables:

| Variable | |
|---|---|
| `BONSAI_ROOTS=dir1:dir2` | spaces to show instead of the detected ones; `dir=shown` sets how a path is displayed (`/tmp/demo=~`) |
| `BONSAI_THREADS=n` | number of scan threads, e.g. inside a job |
| `BONSAI_COLOR=truecolor`, `256` or `16` | force the colors for one run |
| `BONSAI_MMLSQUOTA=command` | the `mmlsquota` to run for the quotas (default: the one in `PATH`, else `/usr/lpp/mmfs/bin/mmlsquota`) |
| `BONSAI_CONFIG=file`, `BONSAI_CACHE=file` | another config file or cache file (tests, or several setups) |

The previous scan is cached in `~/.cache/bonsai-romeo/db.txt`. Only folders of at least 1 MiB or 500 files are kept.
