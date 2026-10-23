/* SCANNER
Explanation:
- This class scans a filesystem path and computes the total sizes of directories.
- The scan runs on several threads (see Walker) and stores its results in a tree (see DirTree).
- A directory is never counted twice, even if it can be reached through two paths.
- Sizes can be read while the scan is running: they are atomics, so get() never waits for the scan.
- Virtual filesystems (like /proc, /sys) and network shares (NFS, SMB) are ignored.
- Only the user's scope is scanned and shown (see UserScope): other users' files are invisible.
- The previous scan is kept in a cache (~/.cache/bonsai-romeo/db.txt):
    - Its totals are shown right away, while the new scan runs (marked as not final).
    - A folder switches to its new totals once it and everything below it was scanned.
    - Growth since the previous scan is computed from it.
    - Saving only replaces what this scan covered: scanning one folder keeps the rest of the cache.
- Public interface includes:
    - scan() to start scanning the root path.
    - totals(path) / get(path) to read a directory (thread-safe).
    - cleanup() for the cleanup suggestions.
    - search(query) to find folders by name.
    - rescan(path) to scan one folder again, growth() for what grew since the previous scan.
- A folder scanned again gets a tree of its own, laid over the main one (see Overlay): the main tree
  never changes shape, so nothing reading it can see a folder half replaced.
*/

#pragma once

#include "dir_tree.hpp"
#include "progress.hpp"
#include "user_scope.hpp"
#include "walker.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

class Scanner {
public:
    /* ScannerRemoveResult
    - Error messages and error codes; when it didn't fail, `reason` may hold a remark about the deletion
    */
    struct ScannerRemoveResult {
        std::string reason;
        bool failed;
        std::string rescan;   // A folder to scan again for exact totals ("": none), see remove()
    };

    /* Metric
    - What get() returns for a directory, and what the menu and the pie show.
    - BYTES: apparent size (sum of file sizes).
    - DISK: allocated on disk (blocks), what the GPFS space quota counts.
    - INODES: number of files and directories, what the GPFS file quota counts.
    */
    enum class Metric { BYTES, DISK, INODES };

    // Totals of a directory
    struct Totals {
        uint64_t bytes = 0;
        uint64_t disk = 0;
        uint64_t inodes = 0;
        int64_t newest = 0;   // Last modification below it (seconds since 1970)
        bool final = false;   // Completely scanned; otherwise from the cache, or still growing
        bool known = false;   // In this scan or in the cache
        bool error = false;   // A folder in it couldn't be read completely: the totals may be too low
    };

    // One cleanup suggestion (see cleanup())
    struct CleanupItem {
        std::string path;
        std::string kind;     // "cache", "build", "old data", "large file", "core dump"...
        std::string hint;     // What it is and how to clean it
        uint64_t disk = 0;
        uint64_t inodes = 0;
        int64_t newest = 0;
        bool is_file = false;
    };

    Scanner(const fs::path& _path, const fs::path& _db_path, UserScope _scope) : path(_path), db_path(_db_path), scope(std::move(_scope)), tree(_path.string()) {};

    void scan();
    void stop();
    bool isDone();

    // Saves the cache (see explanation)
    void snapshot();

    /* remove(path, progress)
    - Deletes a file, or a folder on several threads (see Remover), once all of it was checked deletable.
    - `progress` (optional) shows how far it got, and can stop it: what is left stays, and is scanned again.
    */
    ScannerRemoveResult remove(const fs::path& path, Progress* progress = nullptr);

    void setMetric(Metric m) { metric = m; }
    Metric getMetric() const { return metric; }

    Totals totals(const fs::path& path);
    uint64_t value(const Totals& totals) const;   // For the current metric

    uint64_t get(const fs::path& path);
    uint64_t getSnapped(const fs::path& path);

    // Value of a non-directory entry for the current metric
    uint64_t fileValue(const fs::directory_entry& entry);

    // Formats a value of the current metric (1.20GiB / 1.5M)
    std::string format(uint64_t value, const std::string& spacer = "");

    // Same, at most 5 characters (1.20G / 1.5M), for the sidebar
    std::string formatShort(uint64_t value);

    /* visible(entry)
    - Whether the menu and the pie show this entry.
    - Files: only the user's own, inside their spaces.
    - Directories: only the ones the scan (or the cache) knows, that hold some of the user's data or belong to them.
    */
    bool visible(const fs::directory_entry& entry);

    // Same, from what a listing already knows (see Listing): no call to the file system
    bool visibleEntry(const std::string& path, bool is_dir, bool owned);

    // Value of a listed file for the current metric
    uint64_t fileValue(uint64_t bytes, uint64_t disk) const;

    /* cleanup()
    - What could be cleaned in the user's spaces, biggest first:
        - caches and rebuildable folders (pip / conda / spack / apptainer caches, __pycache__, build folders...)
        - folders where nothing changed for `old_days` days (CLEANUP_OLD_DAYS when 0), and the biggest old files
        - folders holding many files (the file quota), each where its files are, not every folder above it
        - core dumps and the largest files
    - Sorted by size on disk, or by files when the explorer shows files.
    - Works during the scan too, on what was found so far.
    */
    std::vector<CleanupItem> cleanup(int old_days = 0);

    // Old files are kept from this many days on, whatever the threshold chosen later (see cleanup())
    static constexpr int SHORTEST_OLD_DAYS = 90;

    /* rescan(path)
    - Scans `path` again, on the calling thread: after changes made elsewhere, or for a folder made after the scan.
    - Its new totals replace the old ones, and the folders above it get the difference.
    - Refused while the main scan hasn't finished that folder (it would be counted twice).
    */
    struct RescanResult {
        std::string message;
        bool failed;
    };
    RescanResult rescan(const std::string& path, Progress* progress = nullptr);

    // A file made by bonsai-romeo (an archive): counted in its folder and the ones above
    void addFile(const std::string& path);

    /* growth(max_items)
    - Folders that grew the most since the previous scan (the cache), for the current metric.
    - Each growth is listed where it happened: a folder whose growth is mostly one sub-folder's is skipped.
    - Only folders scanned completely, in spaces the previous scan covered.
    */
    struct GrowthItem {
        std::string path;
        int64_t delta;   // Change of the current metric
        Totals now;
        bool is_new;     // Not in the previous scan
    };
    std::vector<GrowthItem> growth(size_t max_items);

    // When the previous scan was saved (seconds since 1970), 0 if there was none
    int64_t previousScan() const { return cache_time; }

    // Folders that couldn't be read completely, and how many there are
    struct ReadError {
        std::string path;
        int error;   // errno
    };
    std::vector<ReadError> readErrors(size_t max_items);
    uint32_t readErrorCount();

    // A folder found by search()
    struct SearchHit {
        std::string path;
        Totals totals;
    };

    /* search(query, max_hits, total)
    - Folders of the user's spaces matching `query`, biggest first (current metric), at most `max_hits`.
    - Case-insensitive. Every word must be in the folder's path (as shown: ~/..., /project/...),
      the last one in the folder's own name: "build" finds every *build* folder, "adc build" only
      those under a path holding "adc".
    - `total` gets the number of matches, even beyond `max_hits`.
    - Works during the scan too, on the folders found so far.
    - `cancelled` is asked now and then: when it says true, the search stops and returns nothing.
    */
    std::vector<SearchHit> search(const std::string& query, size_t max_hits, size_t& total,
                                  const std::function<bool()>& cancelled = {});

    /* Spaces view
    - Without a path to start from, the explorer starts above all the spaces (/gpfs): listing that
      folder would show GPFS's own folders (home, scratch, projet), each holding only the user's one.
    - There, the spaces themselves are listed instead: home, scratch, one entry per project.
    */
    bool isSpacesView(const fs::path& p) const {
        return scope.roots.size() > 1 && p.native() == path.native() && scope.relation(path.native()) == UserScope::Relation::ANCESTOR;
    }

    const UserScope& getScope() const { return scope; }
    uint32_t directories() const { return tree.count(); }

private:
    /* Overlay
    - The tree of a folder scanned again (see rescan()). Its totals replace the main tree's for that
      folder and everything below it; the main tree's old nodes stay where they are, unused.
    - Shared pointers: a reader keeps the overlay it is using, even when a newer scan replaces it.
    */
    struct Overlay {
        std::string root;
        std::unique_ptr<DirTree> tree;
        int64_t started_at = 0;
    };

    // The overlay holding `path` (the deepest one), or nullptr for the main tree
    std::shared_ptr<Overlay> overlayFor(std::string_view path) const;

    // Every folder of the main tree and of the overlays, each from the tree that has its current totals
    using TreeFn = std::function<bool(const std::string&, const DirTree::Node&, const DirTree&)>;
    void walkAll(const TreeFn& fn);
    void walkTree(const DirTree& t, const std::string& root, const std::vector<std::shared_ptr<Overlay>>& list, const TreeFn& fn);

    // Adds `added` and takes `removed` off `dir` and every folder above it, across the trees
    void adjust(std::string dir, const DirTree::Counts& added, const DirTree::Counts& removed);

    // After a folder was scanned again: the read errors marked above it, from what is below them now
    void refreshErrorMarks(std::string dir);

    // A file found by the walker, by its path
    struct FoundFile {
        std::string path;
        uint64_t disk;
        uint64_t bytes;
        int64_t mtime;
    };

    struct Cached {
        uint64_t bytes;
        uint64_t disk;
        uint64_t inodes;
        int64_t newest;
    };

    void loadCache();
    const Cached* cached(const std::string& path) const;

    // Whether this scan has the final word on `path`: scanned completely, or found to be gone
    bool coveredByScan(const std::string& path);

    static std::string filePath(const DirTree& t, uint32_t dir, const std::string& name);

    fs::path path;
    fs::path db_path;

    UserScope scope;
    DirTree tree;

    // Set when the scan has completed, or to make it stop early
    std::atomic<bool> done{false};

    // Set when the program quits: stops a scan of one folder too
    std::atomic<bool> stopping{false};

    // When the main scan started (seconds since 1970): a file changed after it may not have its scanned size
    std::atomic<int64_t> started_at{0};

    mutable std::mutex overlays_mutex;
    std::vector<std::shared_ptr<Overlay>> overlays;
    std::atomic<size_t> overlay_count{0};

    std::atomic<Metric> metric{Metric::BYTES};

    // The previous scan. Filled once by loadCache(), read-only after cache_ready is set
    std::unordered_map<std::string, Cached> cache;
    std::atomic<bool> cache_ready{false};
    int64_t cache_time = 0;   // Set by loadCache(), before cache_ready

    // Largest files and core dumps seen by the walkers
    std::mutex findings_mutex;
    std::vector<FoundFile> largest;
    std::vector<FoundFile> old_files;   // The biggest files unchanged for SHORTEST_OLD_DAYS days or more
    std::vector<FoundFile> cores;

    // Only one thread writes the snapshot file at a time
    std::mutex snapshot_mutex;
};
