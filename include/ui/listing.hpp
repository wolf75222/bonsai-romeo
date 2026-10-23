/* LISTING
- What a folder holds, as the explorer and the pie need it: read from the disk once, then kept a while.
- Reading a folder costs one lstat per entry (owner, size, blocks at once).
- The explorer used to read the folder it shows every 100 ms during the scan, with two stats per file
  (and the pie did the same): for a folder of 20 000 files that was ~245 000 stats per second on the
  file system, 4 times what the scan itself did.
- A listing is kept until `version` changes (after a deletion), for MAX_AGE (to see what other
  programs add), or until the folder is opened again (expire(), unless it was just read: the pie reads
  the folders below the one shown). Folders only keep whether they are the user's: their sizes come
  from the scan, in memory.
- One instance per thread: nothing is shared, nothing is locked.
- get() hands out a shared pointer: the pie reads a listing while it asks for the ones below it,
  which may push old listings out of the cache; the one being read stays alive.
*/

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

class Listing {
public:
    struct Entry {
        std::string path;
        std::string name;
        bool is_dir;
        bool regular;    // A regular file (the pie only draws those)
        bool owned;      // Belongs to the user
        uint64_t bytes;  // Files: apparent size
        uint64_t disk;   // Files: allocated on disk
        int64_t mtime;   // Last modification (seconds since 1970)
    };

    static constexpr std::chrono::seconds MAX_AGE{10};
    static constexpr std::chrono::seconds RECENT{2};
    static constexpr size_t MAX_FOLDERS = 512;

    explicit Listing(uid_t _uid) : uid(_uid) {}

    using Entries = std::shared_ptr<const std::vector<Entry>>;

    // What `dir` holds (empty if it can't be read), symlinks left out like everywhere in bonsai
    Entries get(const fs::path& dir, uint64_t version) {
        const auto now = std::chrono::steady_clock::now();

        auto it = cache.find(dir.native());
        if (it != cache.end() && it->second.version == version && now - it->second.read_at < MAX_AGE)
            return it->second.entries;

        /* Size of the cache
        - The pie opens at most 1 + 3 x 50 folders per pass: far below MAX_FOLDERS
        - Past it, only listings too old to be used again are dropped
        */
        if (cache.size() > MAX_FOLDERS) {
            for (auto old = cache.begin(); old != cache.end();) {
                if (now - old->second.read_at >= MAX_AGE || old->second.version != version)
                    old = cache.erase(old);
                else
                    ++old;
            }
        }

        Cached& cached = cache[dir.native()];
        cached.entries = std::make_shared<const std::vector<Entry>>(read(dir));
        cached.version = version;
        cached.read_at = now;
        return cached.entries;
    }

    // Read again at the next get(), unless read less than `age` ago
    void expire(const fs::path& dir, std::chrono::seconds age) {
        auto it = cache.find(dir.native());
        if (it != cache.end() && std::chrono::steady_clock::now() - it->second.read_at >= age)
            cache.erase(it);
    }

private:
    struct Cached {
        Entries entries;
        uint64_t version = 0;
        std::chrono::steady_clock::time_point read_at;
    };

    std::vector<Entry> read(const fs::path& dir) const {
        std::vector<Entry> out;
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);

        for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const fs::directory_entry& entry = *it;

            std::error_code type_ec;
            if (entry.is_symlink(type_ec))
                continue;

            struct stat st;
            if (lstat(entry.path().c_str(), &st) != 0)
                continue;

            out.push_back(Entry{entry.path().string(), entry.path().filename().string(), S_ISDIR(st.st_mode), S_ISREG(st.st_mode),
                                st.st_uid == uid, static_cast<uint64_t>(st.st_size), static_cast<uint64_t>(st.st_blocks) * 512, st.st_mtime});
        }

        return out;
    }

    uid_t uid;
    std::unordered_map<std::string, Cached> cache;
};
