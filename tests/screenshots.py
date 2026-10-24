#!/usr/bin/env python3
"""Makes the README's GIF and screenshots, from made-up spaces (tests/demo_tree.sh) and quotas (tests/fake-mmlsquota).

    python3 tests/screenshots.py --ssh romeo --dir '~/bonsai-romeo'   # runs there (built there), drawn here

Needs pyte and Pillow here (pip install pyte pillow), only bash, coreutils and python3 on the remote side.
The user shows as "demo": no real name, path or quota of anyone gets in the pictures.
Writes img/bonsai.gif, img/spaces.png, img/explorer.png, img/cleanup.png, img/search.png, img/quotas.png,
img/delete.png and img/grew.png.
"""

import argparse
import os
import shlex
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui import Term, render_png, save_gif  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Names that must never show (the account and groups the pictures are made on, BONSAI_PRIVATE=a,b): checked on every frame
PRIVATE = [name for name in os.environ.get("BONSAI_PRIVATE", "").split(",") if name]


def command(args):
    """The shell command that makes the spaces, scans them once (the previous scan W compares with), then runs bonsai-romeo"""
    root = args.dir
    binary = args.binary or (root + "/build/bonsai-romeo")
    script = (
        "set -e; S=/tmp/bonsai-demo-$(id -un); ROOT={root}; BIN={binary}; "
        "T=$S bash $ROOT/tests/demo_tree.sh >/dev/null; "
        "export USER=demo BONSAI_COLOR=truecolor BONSAI_MMLSQUOTA=$ROOT/tests/fake-mmlsquota "
        "BONSAI_CONFIG=$S/bonsai.conf BONSAI_CACHE=$S/cache.txt "
        "BONSAI_ROOTS=\"$S/home=~:$S/scratch=/scratch_p/user:$S/r12345=/project/r12345\"; "
        "printf 'SIDEBAR_WIDTH = 48\\n' > $S/bonsai.conf; "
        "$BIN --summary >/dev/null; "
        # What grew since that scan
        "fallocate -l 900M $S/scratch/tmp_runs/job_51302/out3.dat; mkdir -p $S/home/datasets/era5/hourly; "
        "for i in 1 2 3; do fallocate -l 150M $S/home/datasets/era5/hourly/h$i.nc; done; "
        "set +e; $BIN; chmod -R u+w $S; rm -rf $S"
    ).format(root=root, binary=binary)
    return ["ssh", "-q", "-tt", args.ssh, "bash -c " + shlex.quote(script)]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--ssh", required=True, help="host to run bonsai-romeo on, drawn here")
    p.add_argument("--dir", default="~/bonsai-romeo", help="the bonsai-romeo folder on that host (built)")
    p.add_argument("--binary", help="bonsai-romeo to run (default: <dir>/build/bonsai-romeo)")
    p.add_argument("--out", default=os.path.join(REPO, "img"))
    p.add_argument("--cols", type=int, default=140)
    p.add_argument("--rows", type=int, default=38)
    args = p.parse_args()

    t = Term(command(args), cols=args.cols, rows=args.rows)
    frames = []
    pngs = {}

    def check():
        screen = t.text()
        for name in PRIVATE:
            if name and name in screen:
                t.close()
                sys.exit("a private name shows on screen: no picture written")

    def shot(seconds, keys=None, settle=0.8, png=None):
        if keys:
            for k in keys.split(","):
                t.send(k)
                t.sleep(0.15)
        t.sleep(settle)
        check()
        frames.append((t.snapshot(), int(seconds * 1000)))
        if png:
            pngs[png] = t.snapshot()

    if not t.wait_for("Explorer", 60):
        print(t.text())
        sys.exit("bonsai-romeo didn't start")
    shot(1.6, None, 0.3)                      # your spaces, sizes from the previous scan (~) while it scans
    t.wait_for("scanned", 30)
    shot(2.2, None, 0.5, "spaces")            # scanned
    shot(1.4, "j,j", 0.6)                     # home
    shot(2.6, "enter", 1.2, "explorer")       # its folders, the chart of what they hold
    shot(1.4, "j", 0.6)                       # .cache: the chart follows the selection
    shot(1.4, "j", 0.6)
    shot(2.0, "f,f", 1.0)                     # number of files (the file quota)
    shot(1.4, "f", 0.8)                       # back to the size
    shot(1.6, "s,s", 0.8)                     # oldest first, with the age of each
    shot(1.2, "s", 0.6)
    shot(2.4, "/,r,u,n", 1.2, "search")       # search folders by name
    shot(1.6, "esc,c", 1.8, "cleanup")        # cleanup suggestions
    shot(1.4, "tab", 0.6)                     # caches and builds
    shot(1.4, "tab", 0.6)                     # old data
    shot(1.6, "tab", 0.6)                     # many files: for the file quota
    shot(2.2, "esc,w", 1.5, "grew")           # what grew since the previous scan
    shot(2.6, "esc,u", 1.2, "quotas")         # quotas: what to remove to get back under a limit
    shot(1.2, "u", 0.6)
    shot(1.4, "j, , ", 0.8)                   # two entries marked
    shot(2.6, "d", 0.8, "delete")             # one confirmation for both (cancelled)
    shot(1.4, "esc,x", 0.6)
    shot(2.4, "?", 0.8)                       # every key
    t.send("\x1b")
    t.sleep(0.3)
    t.send("q")
    t.wait_exit(15)
    t.close()

    os.makedirs(args.out, exist_ok=True)
    images = [render_png(cells) for cells, _ in frames]
    save_gif(images, os.path.join(args.out, "bonsai.gif"), [d for _, d in frames])
    for name, cells in pngs.items():
        render_png(cells, os.path.join(args.out, name + ".png"))
    for name in ["bonsai.gif"] + [n + ".png" for n in pngs]:
        path = os.path.join(args.out, name)
        print("%s: %d KB" % (path, os.path.getsize(path) // 1024))


if __name__ == "__main__":
    main()
