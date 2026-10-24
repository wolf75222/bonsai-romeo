# Development

## Pictures

The README's GIF and the screenshots of these docs come from made-up data, never from a real account:

- `tests/demo_tree.sh`: made-up spaces (a home, a scratch, a project) in `/tmp`, with dates for the ages and the old data.
- `tests/fake-mmlsquota`: made-up quotas, given to bonsai-romeo with `BONSAI_MMLSQUOTA`.
- `tests/tui.py` (from [RSV](https://github.com/wolf75222/RSV)): runs a program in a pseudo-terminal, sends keys, keeps the screen with `pyte` and draws it with Pillow (`pip install pyte pillow`).
- `tests/screenshots.py`: the GIF and the screenshots (all but the progress one). `--ssh romeo --dir '~/bonsai-romeo'` runs bonsai-romeo on ROMEO (built there) and draws it here. `BONSAI_PRIVATE=name,group` stops it if one of these names shows on a frame.
