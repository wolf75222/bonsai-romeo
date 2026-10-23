/* ARCHIVE
- Packs a folder into one .tar file next to it (key T): for the file quota, thousands of files become one.
- No compression: tar only reads and writes, so the login node's CPUs aren't loaded, and it stays fast on GPFS
  (gzip it later in a job if the space matters too).
- The folder is only deleted by the caller once the archive is checked: tar must succeed, then the archive is
  read back and must list every entry the folder holds. Until then it is written as name.tar.part.
- tar is stopped when the program quits: the .part is removed, the folder is left as it was.
*/

#pragma once

#include "progress.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>

class Archive {
public:
    struct Result {
        bool ok = false;
        std::string archive;   // The .tar written, when ok
        std::string error;     // What went wrong, when not
        uint64_t entries = 0;  // Entries of the folder, itself included (and of the archive)
    };

    // A free name next to `dir`: dir.tar, else dir.1.tar, dir.2.tar...
    static std::string archiveName(const std::string& dir);

    /* pack(dir, progress, child)
    - Writes `dir` into its .tar, then checks it. Blocks until done: run it on its own thread.
    - `progress` shows each step and the entries as tar writes them; its `stop` stops it.
    - `child` holds tar's pid while it runs: killing it ends pack() early.
    */
    static Result pack(const std::string& dir, Progress& progress, std::atomic<pid_t>& child);
};
