/* DIR TREE
Explanation:
- Every scanned directory is one node in a tree, instead of one entry in a path -> size hashmap.
- This is the layout idea of fsearch's name index (index.rs): a directory's sub-directories live
  together in one sorted block, so a path is resolved by a binary search per component and no
  full path string is ever stored.
- Built for one scan running on many threads while the UI reads from it:
    - Nodes live in chunks that never move, so a node reference stays valid forever.
    - A directory's block of children is published once, fully built, through an atomic pointer.
    - Sizes are atomics: scanner threads add to them, UI threads read them, nobody takes a lock.
- A node's size is the total of its whole subtree. It grows while the scan is running.
- A node is complete once it and everything below it was scanned: its totals are then final.
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class DirTree {
public:
    static constexpr uint32_t NONE = UINT32_MAX;
    static constexpr uint32_t ROOT = 0;

    /* Block
    - The sub-directories of one directory, sorted by name.
    - Child number i is node `first + i`: ids are handed out in runs, so no id is stored per child.
    - Names are stored back to back, each followed by a '\0'.
    - Never modified once it is attached to its directory.
    */
    struct Block {
        uint32_t first = 0;
        std::vector<uint32_t> offsets; // count + 1 entries into `names`
        std::vector<char> names;

        uint32_t count() const { return static_cast<uint32_t>(offsets.size()) - 1; }
        const char* c_str(uint32_t i) const { return names.data() + offsets[i]; }
        std::string_view name(uint32_t i) const { return {c_str(i), offsets[i + 1] - offsets[i] - 1}; }
    };

    struct Node {
        std::atomic<uint64_t> size{0};            // Bytes in this directory and everything below it
        std::atomic<uint64_t> disk{0};            // Same, as allocated on disk (blocks): what the GPFS quota counts
        std::atomic<uint64_t> inodes{0};          // Files and directories of the user below it (GPFS file quota)
        std::atomic<int64_t> newest{0};           // Last modification (seconds since 1970) of the user's entries below it
        std::atomic<uint32_t> pending{0};         // Sub-directories not complete yet
        std::atomic<bool> complete{false};        // This directory and everything below it was scanned
        std::atomic<const Block*> children{nullptr}; // nullptr: not listed yet, or no sub-directories
        uint32_t parent = NONE;
        std::atomic<bool> removed{false};
    };

    // What one directory adds to itself and to every directory above it
    struct Counts {
        uint64_t bytes = 0;
        uint64_t disk = 0;
        uint64_t inodes = 0;
        int64_t newest = 0;
    };

    explicit DirTree(const std::string& root_path);
    ~DirTree();

    DirTree(const DirTree&) = delete;
    DirTree& operator=(const DirTree&) = delete;

    Node& node(uint32_t id) const;

    /* attach(dir, names)
    - Records the sub-directories of `dir`. `names` are '\0' separated and need no order.
    - Returns the published block (nullptr if the tree is full), whose children can now be scanned.
    - Called at most once per directory, by the thread that listed it.
    */
    const Block* attach(uint32_t dir, const std::vector<char>& names, uint32_t count);

    // Adds / removes `counts` on `dir` and on every directory above it (in one walk up).
    void add(uint32_t dir, const Counts& counts);
    void subtract(uint32_t dir, const Counts& counts);

    /* listed(dir, subdirs)
    - Called once `dir` was read: `subdirs` of its sub-directories are queued.
    - With none (or when it couldn't be read), `dir` is complete, and so is every directory above it
      whose last incomplete sub-directory it was.
    - Must be called before the sub-directories are queued, after `dir`'s own counts were added.
    */
    void listed(uint32_t dir, uint32_t subdirs);

    // Resolves an absolute path to its node, or NONE if it isn't a (known) directory under the root.
    uint32_t find(std::string_view path) const;

    // Writes the absolute path of `id` into `out`.
    void path(uint32_t id, std::string& out) const;

    // Calls fn(path, node) for every directory, parents before children.
    void forEach(const std::function<void(const std::string&, const Node&)>& fn) const;

    // Same, but the children of a directory are skipped when fn returns false for it.
    void walk(const std::function<bool(const std::string&, const Node&)>& fn) const;

    uint32_t count() const;

private:
    /* Node storage
    - Chunk c holds (1024 << c) nodes, so 23 chunks cover every 32 bit id.
    - A chunk is allocated once and never moves or shrinks.
    */
    static constexpr int FIRST_CHUNK_BITS = 10;
    static constexpr int MAX_CHUNKS = 23;

    static int chunkOf(uint32_t id, uint32_t& offset);
    bool reserve(uint64_t first, uint64_t last);

    std::atomic<Node*> chunks[MAX_CHUNKS] = {};
    std::atomic<uint64_t> next_id{1};
    std::mutex chunk_mutex;

    std::string root; // Without trailing slash, so "/" is stored as ""
};
