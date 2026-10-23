# Quotas on ROMEO

Each quota has two limits, one for space and one for the number of files (every file or directory counts as one):

- **soft**: can be exceeded for a grace period of a few days. Once the grace period is over, writing is blocked until you are back under the limit.
- **hard**: can never be exceeded.

| Space | Quota |
|---|---|
| `/home/<user>` | per user |
| `/scratch_p/<user>` | per user |
| `/project/<group>` | per project group, shared by its members (no personal limit) |

Files you put in a project folder must belong to the project group. Files left with your personal group (copied from elsewhere, for example) aren't counted on the project and can make your writes fail. bonsai-romeo detects them and shows the `chgrp` commands that fix it.

Quotas count blocks on disk. In the explorer, `F` switches to the size on disk, which matches them.

![Quota view](../img/quotas.png)

Example of `bonsai-romeo --quota` (`|` marks the soft limit, the end of the bar the hard limit):

```
GPFS quotas - 1 quota(s) need your attention

home - user - /home/user
  Space [####..........................|.]  11%  12.40 GiB / 100.00 GiB  (hard 105.00 GiB)  87.60 GiB left
  Files [####.................|..........]  14%  210 532 / 1 000 000  (hard 1 500 000)  789 468 left

scratch - user - /scratch_p/user
  Space [###################...........|.]  58%  61.20 GiB / 100.00 GiB  (hard 105.00 GiB)  38.80 GiB left
  Files [#####################|##........]  74%  1 120 004 / 1 000 000  (hard 1 500 000)  grace: 4 days left
```

Quotas are read with `mmlsquota`, following the same rules as ROMEO's `/apps/romeotools/myquotas.sh`.
