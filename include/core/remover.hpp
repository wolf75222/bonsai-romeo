/* REMOVER
- Deletes a folder on several threads. GPFS takes about a millisecond per entry deleted: one at a time
  (std::filesystem::remove_all), a folder of a million files took more than twenty minutes.
- check() first looks at every folder: if something can't be deleted, nothing is. It runs on the threads too.
- remove() then has each thread take a folder and delete the files in it, relative to the folder (unlinkat:
  the path isn't looked up again for each file); the folders go last, the deepest first.
- Linux deletes the entries of one folder one at a time, whatever the threads: the gain is across folders.
- Progress (entries deleted, the ones being deleted) can be read while it runs. Once stopped, or after a failure,
  what is left stays as it is.
*/

#pragma once

#include "progress.hpp"

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

class Remover {
public:
    // A folder of the tree and its depth (0: the folder deleted)
    using Folder = std::pair<std::string, uint32_t>;

    struct Check {
        std::vector<std::string> readonly;   // The user's own folders without write permission: made writable first
        std::string blocked;                 // Something that can't be deleted (then nothing is)
        std::vector<Folder> folders;         // Every folder, for remove()
    };

    static Check check(const std::string& root, uid_t uid, Progress& progress);

    struct Result {
        uint64_t removed = 0;   // Entries deleted
        std::string error;      // The first failure ("" when none)
    };

    static Result remove(const std::string& root, const std::vector<Folder>& folders, Progress& progress);

    // How many folders are worked on at a time
    static unsigned threads();
};
