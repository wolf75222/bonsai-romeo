#include "../../include/core/scanner.hpp"

#include "../../include/config/config.hpp"
#include "../../include/utils/format.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <cctype>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

namespace {

// First line of the cache file. Older caches have "path<TAB>bytes" lines and no header
constexpr const char* CACHE_HEADER = "# bonsai-romeo cache 2: path, bytes, disk, inodes, newest";

// Small folders aren't cached: they would make the file big for little use
constexpr uint64_t CACHE_MIN_DISK = 1024 * 1024;
constexpr uint64_t CACHE_MIN_INODES = 500;

constexpr uint64_t MIB = 1024 * 1024;
constexpr uint64_t GIB = 1024 * MIB;

/* Folders that can be cleaned
- name: folder name; parent: name of the folder it must be in (nullptr: anywhere); prefix: name only has to start with it
- The first match wins and nothing below it is suggested again
*/
struct Junk {
    const char* name;
    const char* parent;
    bool prefix;
    const char* kind;
    const char* hint;
};

const Junk JUNK[] = {
    {".cache",            nullptr,       false, "cache",          "Caches of many tools (pip, huggingface, matplotlib...): rebuilt when needed"},
    {"__pycache__",       nullptr,       false, "cache",          "Python bytecode: rebuilt automatically"},
    {".pytest_cache",     nullptr,       false, "cache",          "pytest cache: rebuilt automatically"},
    {".mypy_cache",       nullptr,       false, "cache",          "mypy cache: rebuilt automatically"},
    {".ruff_cache",       nullptr,       false, "cache",          "ruff cache: rebuilt automatically"},
    {".ipynb_checkpoints",nullptr,       false, "cache",          "Jupyter checkpoints of your notebooks"},
    {".ccache",           nullptr,       false, "cache",          "ccache: ccache -C"},
    {".nv",               nullptr,       false, "cache",          "CUDA compute cache: rebuilt by the driver"},
    {".triton",           nullptr,       false, "cache",          "Triton kernel cache: rebuilt when needed"},
    {".npm",              nullptr,       false, "cache",          "npm cache: npm cache clean --force"},
    {".yarn",             nullptr,       false, "cache",          "yarn cache: yarn cache clean"},
    {"registry",          ".cargo",      false, "cache",          "Cargo registry: downloaded again when needed"},
    {"cache",             "pkgs",        false, "conda cache",    "Conda channel indexes (repodata), downloaded again when needed: conda clean -i"},
    {"pkgs",              ".conda",      false, "conda packages", "Downloaded conda packages: conda clean -a"},
    {"pkgs",              "miniconda3",  false, "conda packages", "Downloaded conda packages: conda clean -a"},
    {"pkgs",              "anaconda3",   false, "conda packages", "Downloaded conda packages: conda clean -a"},
    {"pkgs",              "miniforge3",  false, "conda packages", "Downloaded conda packages: conda clean -a"},
    {"pkgs",              "mambaforge",  false, "conda packages", "Downloaded conda packages: conda clean -a"},
    {"cache",             ".apptainer",  false, "apptainer cache","Image layers: apptainer cache clean"},
    {"cache",             ".singularity",false, "apptainer cache","Image layers: singularity cache clean"},
    {"cache",             ".spack",      false, "spack cache",    "Spack downloads and caches: spack clean -a"},
    {"node_modules",      nullptr,       false, "dependencies",   "JavaScript dependencies: reinstalled by npm install"},
    {".tox",              nullptr,       false, "test envs",      "tox environments: rebuilt by tox"},
    {".nox",              nullptr,       false, "test envs",      "nox environments: rebuilt by nox"},
    {".venv",             nullptr,       false, "python env",     "Virtual env: recreated with python -m venv and pip install"},
    {"venv",              nullptr,       false, "python env",     "Virtual env: recreated with python -m venv and pip install"},
    {"CMakeFiles",        nullptr,       false, "build",          "CMake build files: rebuilt by cmake"},
    {"cmake-build-",      nullptr,       true,  "build",          "Build folder: rebuilt by your build"},
    {"build",             nullptr,       false, "build",          "Build folder: rebuilt by your build (check it holds no results)"},
    {"_build",            nullptr,       false, "build",          "Build folder: rebuilt by your build"},
    {"lightning_logs",    nullptr,       false, "ML logs",        "PyTorch Lightning logs and checkpoints"},
    {".vscode-server",    nullptr,       false, "editor server",  "VS Code server: reinstalled on the next connection"},
    {".cursor-server",    nullptr,       false, "editor server",  "Cursor server: reinstalled on the next connection"},
    {"Trash",             "share",       false, "trash",          "Desktop trash: empty it"},
};

const Junk* junk(const std::string& path) {
    const size_t slash = path.rfind('/');
    const std::string_view name = std::string_view(path).substr(slash + 1);

    std::string_view parent;
    if (slash != std::string::npos && slash > 0) {
        const size_t before = path.rfind('/', slash - 1);
        parent = std::string_view(path).substr(before + 1, slash - before - 1);
    }

    for (const Junk& j : JUNK) {
        const bool name_matches = j.prefix ? name.substr(0, std::strlen(j.name)) == j.name : name == j.name;
        if (name_matches && (!j.parent || parent == j.parent))
            return &j;
    }

    return nullptr;
}

/* Before removing a folder: can all of it be removed?
- Removing an entry takes writing in, and entering, the folder holding it. remove_all() stops at the
  first refusal and leaves the folder half deleted: so the whole tree is checked first, and nothing
  is removed when something can't be.
- Folders of the user's own without write permission (made read-only by a tool, or `chmod -w`) can
  be fixed: they are listed to be made writable. Someone else's can't: `blocked` names one.
- Only folders are looked at (one readdir each); files need a stat only in sticky folders.
*/
struct RemovalCheck {
    std::vector<std::string> readonly;
    std::string blocked;
};

RemovalCheck checkRemoval(const std::string& root, uid_t uid) {
    RemovalCheck out;
    std::vector<std::string> stack{root};

    while (!stack.empty()) {
        const std::string dir = std::move(stack.back());
        stack.pop_back();

        struct stat st;
        if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        const bool usable = (st.st_mode & S_IRWXU) == S_IRWXU;
        if (st.st_uid == uid) {
            if (!usable) out.readonly.push_back(dir);
        } else if (access(dir.c_str(), R_OK | W_OK | X_OK) != 0) {
            out.blocked = dir;
            return out;
        }

        // An unreadable folder of the user's can only be listed once fixed: it is taken as it comes
        DIR* listing = opendir(dir.c_str());
        if (!listing)
            continue;

        // In a sticky folder (like /tmp) only one's own entries can be removed
        const bool sticky = (st.st_mode & S_ISVTX) && st.st_uid != uid;

        while (const dirent* entry = readdir(listing)) {
            const char* name = entry->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
                continue;

            std::string child = dir + "/" + name;
            unsigned char type = entry->d_type;

            if (type == DT_UNKNOWN || sticky) {
                struct stat cs;
                if (lstat(child.c_str(), &cs) != 0)
                    continue;
                if (sticky && cs.st_uid != uid) {
                    out.blocked = child;
                    closedir(listing);
                    return out;
                }
                type = S_ISDIR(cs.st_mode) ? DT_DIR : DT_REG;
            }

            if (type == DT_DIR)
                stack.push_back(std::move(child));
        }

        closedir(listing);
    }

    return out;
}

}

void Scanner::loadCache() {
    std::ifstream in(this->db_path);
    std::string line;
    bool v2 = false;

    auto number = [](std::string_view s, auto& out) {
        try {
            out = static_cast<std::remove_reference_t<decltype(out)>>(std::stoll(std::string(s)));
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };

    while (in.is_open() && std::getline(in, line)) {
        if (line.empty()) continue;

        if (line[0] == '#') {
            v2 = v2 || line == CACHE_HEADER;
            continue;
        }

        // Numbers are taken from the end: a path may hold any character
        Cached c{0, 0, 0, 0};
        size_t end = line.size();
        const int fields = v2 ? 4 : 1;
        std::string_view parts[4];
        bool ok = true;

        for (int i = fields - 1; i >= 0 && ok; i--) {
            const size_t tab = line.rfind('\t', end - 1);
            if (tab == std::string::npos || tab == 0) { ok = false; break; }
            parts[i] = std::string_view(line).substr(tab + 1, end - tab - 1);
            end = tab;
        }

        if (!ok) continue;

        if (v2) {
            ok = number(parts[0], c.bytes) && number(parts[1], c.disk) && number(parts[2], c.inodes) && number(parts[3], c.newest);
        } else {
            ok = number(parts[0], c.bytes);
        }

        if (ok)
            cache[line.substr(0, end)] = c;
    }

    cache_ready.store(true, std::memory_order_release);
}

const Scanner::Cached* Scanner::cached(const std::string& path) const {
    if (!cache_ready.load(std::memory_order_acquire))
        return nullptr;

    auto it = cache.find(path);
    return it == cache.end() ? nullptr : &it->second;
}

bool Scanner::coveredByScan(const std::string& p) {
    if (scope.relation(p) == UserScope::Relation::OUTSIDE)
        return false;

    // The deepest folder of the tree on the way to `p`: if it is complete, `p` was written, or it is gone
    std::string probe = p;
    while (true) {
        const uint32_t id = tree.find(probe);
        if (id != DirTree::NONE)
            return tree.node(id).complete.load(std::memory_order_acquire);

        const size_t slash = probe.rfind('/');
        if (slash == std::string::npos || slash == 0)
            return false;

        probe.resize(slash);
    }
}

void Scanner::snapshot() {
    std::lock_guard<std::mutex> lock(snapshot_mutex);

    std::error_code ec;
    if (this->db_path.has_parent_path()) {
        fs::create_directories(this->db_path.parent_path(), ec);
    }

    // Written next to it, then renamed: a crash or a second bonsai never leaves half a cache
    const fs::path tmp = this->db_path.string() + "." + std::to_string(getpid()) + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open()) return;

    out << CACHE_HEADER << '\n';

    // 1. What this scan found, for the folders it scanned completely
    tree.forEach([&out](const std::string& path, const DirTree::Node& dir) {
        if (!dir.complete.load(std::memory_order_acquire))
            return;

        const uint64_t disk = dir.disk.load(std::memory_order_relaxed);
        const uint64_t inodes = dir.inodes.load(std::memory_order_relaxed);
        if (disk < CACHE_MIN_DISK && inodes < CACHE_MIN_INODES)
            return;

        out << path << '\t' << dir.size.load(std::memory_order_relaxed) << '\t' << disk << '\t' << inodes
            << '\t' << dir.newest.load(std::memory_order_relaxed) << '\n';
    });

    // 2. The rest of the previous cache: other spaces, or folders this scan didn't finish
    if (cache_ready.load(std::memory_order_acquire)) {
        for (const auto& [path, c] : cache) {
            if (!coveredByScan(path))
                out << path << '\t' << c.bytes << '\t' << c.disk << '\t' << c.inodes << '\t' << c.newest << '\n';
        }
    }

    out.close();
    if (out.good())
        fs::rename(tmp, this->db_path, ec);
    if (ec || !out.good())
        fs::remove(tmp, ec);
}

Scanner::Totals Scanner::totals(const fs::path& path) {
    Totals t;

    const uint32_t id = tree.find(path.native());
    if (id != DirTree::NONE) {
        const DirTree::Node& n = tree.node(id);

        t.final = n.complete.load(std::memory_order_acquire);
        t.bytes = n.size.load(std::memory_order_relaxed);
        t.disk = n.disk.load(std::memory_order_relaxed);
        t.inodes = n.inodes.load(std::memory_order_relaxed);
        t.newest = n.newest.load(std::memory_order_relaxed);
        t.known = true;

        if (t.final)
            return t;
    }

    // Still being scanned: the previous scan's totals, never below what this one found so far
    if (!done.load()) {
        if (const Cached* c = cached(path.native())) {
            t.bytes = std::max(t.bytes, c->bytes);
            t.disk = std::max(t.disk, c->disk);
            t.inodes = std::max(t.inodes, c->inodes);
            t.newest = std::max(t.newest, c->newest);
            t.known = true;
        }
    }

    return t;
}

uint64_t Scanner::value(const Totals& t) const {
    switch (metric.load()) {
        case Metric::DISK:   return t.disk;
        case Metric::INODES: return t.inodes;
        case Metric::BYTES:
        default:             return t.bytes;
    }
}

uint64_t Scanner::get(const fs::path& path) {
    return value(totals(path));
}

uint64_t Scanner::getSnapped(const fs::path& path) {
    if (!Config::get().ENABLE_FOLDER_SIZE_PERCENTAGES)
        return 0;

    // Growth only makes sense against final totals
    const uint32_t id = tree.find(path.native());
    if (id == DirTree::NONE || !tree.node(id).complete.load(std::memory_order_acquire))
        return 0;

    const Cached* c = cached(path.native());
    if (!c)
        return 0;

    return value(Totals{c->bytes, c->disk, c->inodes, c->newest, true, true});
}

uint64_t Scanner::fileValue(const fs::directory_entry& entry) {
    switch (metric.load()) {
        case Metric::INODES:
            return 1;

        case Metric::DISK: {
            struct stat st;
            return lstat(entry.path().c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_blocks) * 512 : 0;
        }

        case Metric::BYTES:
        default: {
            std::error_code ec;
            uint64_t size = entry.file_size(ec);
            return ec ? 0 : size;
        }
    }
}

std::string Scanner::formatShort(uint64_t value) {
    return metric == Metric::INODES ? FormatUtils::toCount(value) : FormatUtils::toCompact(value);
}

std::string Scanner::format(uint64_t value, const std::string& spacer) {
    return metric == Metric::INODES ? FormatUtils::toCount(value, spacer) : FormatUtils::toReadable(value, spacer);
}

bool Scanner::visible(const fs::directory_entry& entry) {
    std::error_code ec;

    if (entry.is_symlink(ec))
        return false;

    if (entry.is_directory(ec)) {
        const Totals t = totals(entry.path());
        if (!t.known)
            return false;

        return t.bytes > 0 || t.inodes > 0 || scope.owns(entry.path());
    }

    return scope.relation(entry.path().native()) == UserScope::Relation::INSIDE
        && scope.relation(entry.path().parent_path().native()) == UserScope::Relation::INSIDE
        && scope.owns(entry.path());
}

std::string Scanner::filePath(uint32_t dir, const std::string& name) const {
    std::string out;
    tree.path(dir, out);
    if (out != "/") out += '/';
    return out + name;
}

std::vector<Scanner::CleanupItem> Scanner::cleanup() {
    std::vector<CleanupItem> items;

    const int64_t now = std::time(nullptr);
    const int64_t old_after = static_cast<int64_t>(std::max(1, Config::get().CLEANUP_OLD_DAYS)) * 86400;

    // Folders: the first match on the way down wins, nothing below it is suggested again
    tree.walk([&](const std::string& path, const DirTree::Node& node) {
        if (scope.relation(path) != UserScope::Relation::INSIDE)
            return true;

        const uint64_t disk = node.disk.load(std::memory_order_relaxed);
        const uint64_t inodes = node.inodes.load(std::memory_order_relaxed);
        const int64_t newest = node.newest.load(std::memory_order_relaxed);

        // Nothing of the user below it
        if (disk == 0 && inodes == 0)
            return false;

        if (const Junk* j = junk(path)) {
            if (disk >= MIB || inodes >= 1000)
                items.push_back(CleanupItem{path, j->kind, j->hint, disk, inodes, newest, false});
            return false;
        }

        if (disk >= GIB && newest > 0 && now - newest > old_after) {
            items.push_back(CleanupItem{path, "old data", "Nothing changed in it for " + FormatUtils::toAge(now - newest)
                                        + ": archive it elsewhere (no backups on ROMEO) or delete it", disk, inodes, newest, false});
            return false;
        }

        return true;
    });

    // Files: core dumps and the largest files
    {
        std::lock_guard<std::mutex> lock(findings_mutex);

        for (const auto& hit : findings.cores)
            items.push_back(CleanupItem{filePath(hit.dir, hit.name), "core dump", "Memory dump of a crashed program: safe to delete", hit.disk, 1, hit.mtime, true});

        for (const auto& hit : findings.largest) {
            if (hit.disk < 100 * MIB) break;

            // Already listed as a core dump
            const bool core = std::any_of(findings.cores.begin(), findings.cores.end(), [&hit](const Walker::FileHit& c) {
                return c.dir == hit.dir && c.name == hit.name;
            });
            if (!core)
                items.push_back(CleanupItem{filePath(hit.dir, hit.name), "large file", "One of your largest files", hit.disk, 1, hit.mtime, true});
        }
    }

    std::stable_sort(items.begin(), items.end(), [](const CleanupItem& a, const CleanupItem& b) {
        return a.disk > b.disk;
    });

    // A large file inside a suggested folder is already covered by it
    std::vector<bool> covered(items.size(), false);
    for (size_t i = 0; i < items.size(); i++) {
        if (!items[i].is_file) continue;

        const std::string& file = items[i].path;
        covered[i] = std::any_of(items.begin(), items.end(), [&file](const CleanupItem& other) {
            return !other.is_file && file.size() > other.path.size()
                && file.compare(0, other.path.size(), other.path) == 0 && file[other.path.size()] == '/';
        });
    }

    std::vector<CleanupItem> kept;
    for (size_t i = 0; i < items.size(); i++) {
        if (!covered[i])
            kept.push_back(std::move(items[i]));
    }

    if (kept.size() > 300)
        kept.resize(300);

    return kept;
}

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// `needle` is already in lower case
bool containsNoCase(std::string_view hay, const std::string& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    }) != hay.end();
}

}

// Whether `word` (lower case) is in the path as display() shows it, without building it for every folder
static bool shownPathContains(const UserScope& scope, const std::string& real, const std::string& word) {
    for (const auto& [prefix, shown] : scope.aliases) {
        if (!UserScope::isUnder(real, prefix))
            continue;

        const std::string_view rest = std::string_view(real).substr(prefix.size());
        if (containsNoCase(shown, word) || containsNoCase(rest, word))
            return true;

        // A word across both parts ("~/adc"): rare, so the shown path is only built then
        return word.find('/') != std::string::npos && containsNoCase(shown + std::string(rest), word);
    }

    return containsNoCase(real, word);
}

std::vector<Scanner::SearchHit> Scanner::search(const std::string& query, size_t max_hits, size_t& total,
                                                const std::function<bool()>& cancelled) {
    total = 0;

    std::vector<std::string> words;
    {
        std::istringstream in(query);
        std::string word;
        while (in >> word)
            words.push_back(lower(word));
    }

    if (words.empty() || max_hits == 0)
        return {};

    const std::string& last = words.back();

    // The best `max_hits` so far, smallest first (min-heap on the current metric)
    std::vector<SearchHit> hits;
    auto smaller = [this](const SearchHit& a, const SearchHit& b) { return value(a.totals) > value(b.totals); };

    size_t visited = 0;
    bool aborted = false;

    tree.walk([&](const std::string& path, const DirTree::Node& node) {
        // A newer search is waiting: every folder left is skipped, so the walk ends right away
        if (aborted)
            return false;
        if ((++visited & 4095) == 0 && cancelled && cancelled()) {
            aborted = true;
            return false;
        }

        // The last word must be in the folder name: cheap, so it goes first
        const size_t slash = path.rfind('/');
        const std::string_view name = std::string_view(path).substr(slash == std::string::npos ? 0 : slash + 1);
        if (name.empty() || !containsNoCase(name, last))
            return true;

        if (scope.relation(path) != UserScope::Relation::INSIDE)
            return true;

        // The other words anywhere in the path, as the user sees it
        for (size_t i = 0; i + 1 < words.size(); i++) {
            if (!shownPathContains(scope, path, words[i]))
                return true;
        }

        total++;

        const Totals totals{node.size.load(std::memory_order_relaxed), node.disk.load(std::memory_order_relaxed),
                            node.inodes.load(std::memory_order_relaxed), node.newest.load(std::memory_order_relaxed),
                            node.complete.load(std::memory_order_acquire), true};

        // The path is only copied for the folders that are kept
        if (hits.size() < max_hits) {
            hits.push_back(SearchHit{path, totals});
            std::push_heap(hits.begin(), hits.end(), smaller);
        } else if (value(totals) > value(hits.front().totals)) {
            std::pop_heap(hits.begin(), hits.end(), smaller);
            hits.back().path.assign(path);
            hits.back().totals = totals;
            std::push_heap(hits.begin(), hits.end(), smaller);
        }

        return true;
    });

    if (aborted) {
        total = 0;
        return {};
    }

    std::sort_heap(hits.begin(), hits.end(), smaller);
    return hits;
}

Scanner::ScannerRemoveResult Scanner::remove(const fs::path& path) {
    // Shared spaces (/project) hold other people's data: never touch it
    if (!scope.owns(path) || scope.relation(path.native()) != UserScope::Relation::INSIDE) {
        return ScannerRemoveResult{"This isn't yours: only your own files can be removed.", true};
    }

    // Nor a whole space at once
    if (scope.isRoot(path.native())) {
        return ScannerRemoveResult{"This is one of your spaces: open it and delete what is inside instead.", true};
    }

    /* During the scan too
    - Upstream refused ("Scanner hasn't completed yet"): what is removed may not be fully counted yet,
      so taking it off the folders above could leave wrong totals
    - Instead the removal waits for the scan where it has to (see below), on the deleting thread:
      false when the scan stopped first (the program is quitting)
    */
    auto waitFor = [this](const std::function<bool()>& ready) {
        while (!ready()) {
            if (done.load())
                return ready();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    };

    std::error_code ec;
    const bool is_dir = fs::is_directory(fs::symlink_status(path, ec));
    const uint32_t parent = tree.find(path.parent_path().native());

    // Removing an entry takes writing in its folder (left as it is: it isn't part of what is removed)
    if (access(path.parent_path().c_str(), W_OK | X_OK) != 0) {
        return ScannerRemoveResult{"Nothing was deleted: " + scope.display(path.parent_path().string())
                                   + " is read-only (chmod u+w on it to delete what it holds).", true};
    }

    // A folder: everything in it must be removable, or nothing is removed (see checkRemoval())
    size_t made_writable = 0;
    if (is_dir) {
        const RemovalCheck check = checkRemoval(path.string(), scope.uid);

        if (!check.blocked.empty()) {
            return ScannerRemoveResult{"Nothing was deleted: " + scope.display(check.blocked)
                                       + " isn't yours and can't be emptied by you.", true};
        }

        for (const auto& dir : check.readonly) {
            struct stat st;
            if (lstat(dir.c_str(), &st) == 0 && chmod(dir.c_str(), (st.st_mode & 07777) | S_IRWXU) == 0)
                made_writable++;
        }
    }

    /* A file is counted when its folder is read
    - Removed before that, it would be taken off totals it was never added to: wait (it is nearly always read already)
    - A file with other hard links frees nothing, and was counted once, maybe elsewhere: nothing to take off
    */
    DirTree::Counts file_counts;
    if (!is_dir) {
        if (parent != DirTree::NONE)
            waitFor([&] { return tree.node(parent).read.load(std::memory_order_acquire); });

        struct stat st;
        if (lstat(path.c_str(), &st) == 0 && st.st_nlink <= 1) {
            file_counts.bytes = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
            file_counts.disk = static_cast<uint64_t>(st.st_blocks) * 512;
            file_counts.inodes = 1;
        }
    }

    try {
        if (is_dir) {
            fs::remove_all(path, ec);
        } else {
            fs::remove(path, ec);
        }
    } catch (const fs::filesystem_error&) {}

    // Everything was checked beforehand: this is unexpected (the disk changed meanwhile, an I/O error...)
    if (ec) {
        return ScannerRemoveResult{ec.message() + (is_dir ? ": part of it may already be deleted, and its size is out of date until the next scan." : ""), true};
    }

    if (!is_dir) {
        if (parent != DirTree::NONE)
            tree.subtract(parent, file_counts);
    } else {
        /* A folder: everything the scan counted in it leaves every folder above it
        - Its totals are final once it is complete: during the scan that comes fast, its content being gone
        - Looked up after the removal: the scan may have just found it
        */
        const uint32_t dir = tree.find(path.native());

        if (dir != DirTree::NONE) {
            waitFor([&] { return tree.node(dir).complete.load(std::memory_order_acquire); });

            DirTree::Node& node = tree.node(dir);
            const DirTree::Counts counts{node.size.load(), node.disk.load(), node.inodes.load(), 0};

            node.removed = true;
            if (node.parent != DirTree::NONE)
                tree.subtract(node.parent, counts);
        }
    }

    this->snapshot();

    // Not an error: said next to the deletion
    if (made_writable > 0)
        return ScannerRemoveResult{std::to_string(made_writable) + " read-only folder" + (made_writable > 1 ? "s" : "") + " in it made writable first", false};

    return ScannerRemoveResult{"", false};
}

void Scanner::scan() {
    // The previous scan is read while this one starts: it only delays what is shown, never the scan
    std::thread loader(&Scanner::loadCache, this);

    // BONSAI_THREADS overrides the config, e.g. to use more threads inside a job
    int threads = Config::get().SCANNER_THREADS;
    if (const char* env = std::getenv("BONSAI_THREADS"); env && *env)
        threads = std::atoi(env);

    Walker walker(tree, scope, done, std::max(0, threads));
    walker.run();

    {
        std::lock_guard<std::mutex> lock(findings_mutex);
        findings = walker.findings();
    }

    loader.join();
    this->done = true;
}

void Scanner::stop() {
    this->done = true;
}

bool Scanner::isDone() {
    return this->done;
}
