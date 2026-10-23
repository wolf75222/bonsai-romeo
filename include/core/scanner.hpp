/* SCANNER
Explanation:
- This class scans a filesystem path and computes the total sizes of directories.
- The scan runs on several threads (see Walker) and stores its results in a tree (see DirTree).
- A directory is never counted twice, even if it can be reached through two paths.
- Sizes can be read while the scan is running: they are atomics, so get() never waits for the scan.
- Virtual filesystems (like /proc, /sys) and network shares (NFS, SMB) are ignored.
- Only the user's scope is scanned and shown (see UserScope): other users' files are invisible.
- Public interface includes:
    - scan() to start scanning the root path.
    - get(path) to retrieve the total size of a directory (thread-safe).
*/

#pragma once

#include "dir_tree.hpp"
#include "user_scope.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

class Scanner {
public:
    /* ScannerRemoveResult
    - Purely used for error messages and error codes
    */
    struct ScannerRemoveResult {
        std::string reason;
        bool failed;
    };

    struct SnapshotDiff {
        uint64_t old_size;
        uint64_t new_size;
        double percent_change; // (new-old)/old * 100, 0 if old==0 and new==0
    };

    Scanner(const fs::path& _path, const fs::path& _db_path, UserScope _scope) : path(_path), db_path(_db_path), scope(std::move(_scope)), tree(_path.string()) {};

    void scan();
    void stop();
    bool isDone();

    void snapshot();
    void loadSnapshot();

    ScannerRemoveResult remove(const fs::path& path);

    /* Metric
    - What get() returns for a directory, and what the menu and the pie show.
    - BYTES: total size. INODES: number of files and directories (what the GPFS file quota counts).
    */
    enum class Metric { BYTES, INODES };

    void setMetric(Metric m) { metric = m; }
    Metric getMetric() const { return metric; }

    uint64_t get(const fs::path& path);
    uint64_t getSnapped(const fs::path& path);
    uint64_t getBytes(const fs::path& path);
    uint64_t getInodes(const fs::path& path);

    // Value of a non-directory entry for the current metric
    uint64_t fileValue(const fs::directory_entry& entry);

    // Formats a value of the current metric (1.20GiB / 1.5M)
    std::string format(uint64_t value, const std::string& spacer = "");

    // Same, at most 5 characters (1.20G / 1.5M), for the sidebar
    std::string formatShort(uint64_t value);

    /* visible(entry)
    - Whether the menu and the pie show this entry.
    - Files: only the user's own, inside their spaces.
    - Directories: only the ones the scan kept, that hold some of the user's bytes or belong to them.
    */
    bool visible(const fs::directory_entry& entry);

private:
    fs::path path;
    fs::path db_path;

    UserScope scope;
    DirTree tree;

    // Set when the scan has completed, or to make it stop early
    std::atomic<bool> done{false};

    std::atomic<Metric> metric{Metric::BYTES};

    // Only one thread writes the snapshot file at a time
    std::mutex snapshot_mutex;
};
