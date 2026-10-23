#include "../../include/core/scanner.hpp"

#include "../../include/config/config.hpp"
#include "../../include/core/remover.hpp"
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
#include <optional>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <unordered_set>

namespace {

// First line of the cache file. Older caches have "path<TAB>bytes" lines and no header
constexpr const char* CACHE_HEADER = "# bonsai-romeo cache 2: path, bytes, disk, inodes, newest";

// Small folders aren't cached: they would make the file big for little use
constexpr uint64_t CACHE_MIN_DISK = 1024 * 1024;
constexpr uint64_t CACHE_MIN_INODES = 500;

constexpr uint64_t MIB = 1024 * 1024;
constexpr uint64_t GIB = 1024 * MIB;

// A folder holding at least this many files is suggested for the file quota (see cleanup())
constexpr uint64_t MANY_FILES = 10000;

// A sub-folder holding this share of a folder's files (or growth) is where they are, not the folder
constexpr double MOSTLY = 0.8;

// Files last changed before this count as old for the walkers: the shortest threshold the cleanup view offers
int64_t oldCutoff() {
    const int days = std::min(Scanner::SHORTEST_OLD_DAYS, std::max(1, Config::get().CLEANUP_OLD_DAYS));
    return static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(days) * 86400;
}

// Scan threads: the config, or BONSAI_THREADS (e.g. inside a job); 0 picks them automatically
unsigned scanThreads() {
    int threads = Config::get().SCANNER_THREADS;
    if (const char* env = std::getenv("BONSAI_THREADS"); env && *env)
        threads = std::atoi(env);
    return static_cast<unsigned>(std::max(0, threads));
}

DirTree::Counts countsOf(const DirTree::Node& n) {
    return DirTree::Counts{n.size.load(std::memory_order_relaxed), n.disk.load(std::memory_order_relaxed),
                           n.inodes.load(std::memory_order_relaxed), n.newest.load(std::memory_order_relaxed)};
}

std::string parentOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || path == "/") return "";
    return slash == 0 ? "/" : path.substr(0, slash);
}

std::string childOf(const std::string& dir, std::string_view name) {
    std::string out = dir == "/" ? "" : dir;
    out += '/';
    out += name;
    return out;
}

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

}

void Scanner::loadCache() {
    struct stat st;
    if (stat(this->db_path.c_str(), &st) == 0)
        cache_time = st.st_mtime;

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
        const auto ov = overlayFor(probe);
        const DirTree& t = ov ? *ov->tree : tree;
        const uint32_t id = t.find(probe);
        if (id != DirTree::NONE)
            return t.node(id).complete.load(std::memory_order_acquire);

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

    // 1. What this scan found, for the folders it scanned completely (from the latest scan of each)
    walkAll([&out](const std::string& path, const DirTree::Node& dir, const DirTree&) {
        if (!dir.complete.load(std::memory_order_acquire))
            return true;

        const uint64_t disk = dir.disk.load(std::memory_order_relaxed);
        const uint64_t inodes = dir.inodes.load(std::memory_order_relaxed);
        if (disk < CACHE_MIN_DISK && inodes < CACHE_MIN_INODES)
            return true;

        out << path << '\t' << dir.size.load(std::memory_order_relaxed) << '\t' << disk << '\t' << inodes
            << '\t' << dir.newest.load(std::memory_order_relaxed) << '\n';
        return true;
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

    // A folder scanned again: its own tree (kept alive while it is read)
    const auto ov = overlayFor(path.native());
    const DirTree& in = ov ? *ov->tree : tree;

    const uint32_t id = in.find(path.native());
    if (id != DirTree::NONE) {
        const DirTree::Node& n = in.node(id);

        t.final = n.complete.load(std::memory_order_acquire);
        t.bytes = n.size.load(std::memory_order_relaxed);
        t.disk = n.disk.load(std::memory_order_relaxed);
        t.inodes = n.inodes.load(std::memory_order_relaxed);
        t.newest = n.newest.load(std::memory_order_relaxed);
        t.error = n.error_below.load(std::memory_order_relaxed);
        t.known = true;

        if (t.final)
            return t;
    }

    // Still being scanned: the previous scan's totals, never below what this one found so far
    if (!ov && !done.load()) {
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
    const auto ov = overlayFor(path.native());
    const DirTree& in = ov ? *ov->tree : tree;
    const uint32_t id = in.find(path.native());
    if (id == DirTree::NONE || !in.node(id).complete.load(std::memory_order_acquire))
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

bool Scanner::visibleEntry(const std::string& entry, bool is_dir, bool owned) {
    if (is_dir) {
        const Totals t = totals(entry);
        if (t.known)
            return t.bytes > 0 || t.inodes > 0 || owned;

        // Not in the scan yet (made after it, or not reached): shown when it is the user's, inside a space
        return owned && scope.relation(entry) == UserScope::Relation::INSIDE;
    }

    const size_t slash = entry.rfind('/');
    const std::string parent = slash == std::string::npos ? entry : entry.substr(0, slash);
    return owned && scope.relation(entry) == UserScope::Relation::INSIDE && scope.relation(parent) == UserScope::Relation::INSIDE;
}

uint64_t Scanner::fileValue(uint64_t bytes, uint64_t disk) const {
    switch (metric.load()) {
        case Metric::INODES: return 1;
        case Metric::DISK:   return disk;
        case Metric::BYTES:
        default:             return bytes;
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

std::string Scanner::filePath(const DirTree& t, uint32_t dir, const std::string& name) {
    std::string out;
    t.path(dir, out);
    if (out != "/") out += '/';
    return out + name;
}

std::vector<Scanner::CleanupItem> Scanner::cleanup(int old_days) {
    std::vector<CleanupItem> items;

    const int64_t now = std::time(nullptr);
    const int days = old_days > 0 ? old_days : std::max(1, Config::get().CLEANUP_OLD_DAYS);
    const int64_t old_after = static_cast<int64_t>(days) * 86400;

    // Folders: the first match on the way down wins, nothing below it is suggested again
    walkAll([&](const std::string& path, const DirTree::Node& node, const DirTree&) {
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

        // Only once scanned completely: a recent file not reached yet would make it look untouched
        if ((disk >= 100 * MIB || inodes >= MANY_FILES) && newest > 0 && now - newest > old_after && node.complete.load(std::memory_order_acquire)) {
            items.push_back(CleanupItem{path, "old data", "Nothing changed in it for " + FormatUtils::toAge(now - newest)
                                        + ": archive it elsewhere (no backups on ROMEO) or delete it", disk, inodes, newest, false});
            return false;
        }

        return true;
    });

    /* Many files, for the file quota
    - A second pass, so they never hide a cache or old data below them; nothing inside those is suggested again
    - Listed where the files are: a folder whose files are mostly in one to three big sub-folders is skipped for them,
      and a whole space never is (its folders are)
    */
    std::unordered_set<std::string> suggested;
    const std::vector<CleanupItem> first_pass = items;
    for (const auto& item : items)
        suggested.insert(item.path);

    walkAll([&](const std::string& path, const DirTree::Node& node, const DirTree& t) {
        const UserScope::Relation relation = scope.relation(path);
        if (relation != UserScope::Relation::INSIDE)
            return relation == UserScope::Relation::ANCESTOR;

        const uint64_t inodes = node.inodes.load(std::memory_order_relaxed);
        if (inodes < MANY_FILES || suggested.count(path))
            return false;
        if (scope.isRoot(path))
            return true;

        // Its big sub-folders: when one to three of them hold most of its files, they are listed instead
        uint64_t in_big = 0;
        int big = 0;
        if (const DirTree::Block* block = node.children.load(std::memory_order_acquire)) {
            for (uint32_t i = 0; i < block->count(); i++) {
                const DirTree::Node& child = t.node(block->first + i);
                if (child.removed.load(std::memory_order_relaxed))
                    continue;

                // A sub-folder scanned again has its totals in its own tree
                const uint64_t files = overlay_count.load(std::memory_order_acquire) > 0
                    ? totals(childOf(path, block->name(i))).inodes : child.inodes.load(std::memory_order_relaxed);
                if (files >= MANY_FILES) {
                    in_big += files;
                    big++;
                }
            }
        }

        if (big > 0 && big <= 3 && static_cast<double>(in_big) >= static_cast<double>(inodes) * MOSTLY)
            return true;

        // What is already suggested inside it isn't counted again
        uint64_t files = inodes;
        uint64_t disk = node.disk.load(std::memory_order_relaxed);
        for (const auto& other : first_pass) {
            if (UserScope::isUnder(other.path, path)) {
                files -= std::min(files, other.inodes);
                disk -= std::min(disk, other.disk);
            }
        }
        if (files < MANY_FILES)
            return true;

        // Spread over many big sub-folders: one archive of all of it is rarely what is wanted
        const std::string hint = big > 3
            ? FormatUtils::withSeparators(files) + " files, spread over " + std::to_string(big) + " big folders: open it, F shows files, then pack (T) or delete those"
            : FormatUtils::withSeparators(files) + " files: pack them into one file (T in the explorer) to free your file quota, or delete what you no longer need";
        items.push_back(CleanupItem{path, "many files", hint, disk, files, node.newest.load(std::memory_order_relaxed), false});
        return false;
    });

    // Files: core dumps and the largest files
    {
        std::lock_guard<std::mutex> lock(findings_mutex);

        // Files deleted since the scan (here or elsewhere) aren't suggested again
        auto gone = [](const std::string& file) {
            struct stat st;
            return lstat(file.c_str(), &st) != 0;
        };

        for (const auto& hit : cores) {
            if (!gone(hit.path))
                items.push_back(CleanupItem{hit.path, "core dump", "Memory dump of a crashed program: safe to delete", hit.disk, 1, hit.mtime, true});
        }

        auto core = [this](const FoundFile& hit) {
            return std::any_of(cores.begin(), cores.end(), [&hit](const FoundFile& c) { return c.path == hit.path; });
        };
        auto old = [&](const FoundFile& hit) { return hit.mtime > 0 && now - hit.mtime > old_after; };

        // An old one is listed as such, below
        for (const auto& hit : largest) {
            if (hit.disk < 100 * MIB) break;
            if (!core(hit) && !old(hit) && !gone(hit.path))
                items.push_back(CleanupItem{hit.path, "large file", "One of your largest files", hit.disk, 1, hit.mtime, true});
        }

        // The biggest files unchanged for long (biggest first: below 1 MiB, one at a time isn't worth it)
        for (const auto& hit : old_files) {
            if (hit.disk < MIB) break;
            if (old(hit) && !core(hit) && !gone(hit.path))
                items.push_back(CleanupItem{hit.path, "old file", "Unchanged for " + FormatUtils::toAge(now - hit.mtime)
                                            + ": archive it elsewhere (no backups on ROMEO) or delete it", hit.disk, 1, hit.mtime, true});
        }
    }

    // Biggest first: on disk, or by files when the explorer shows files (the file quota)
    const bool by_files = metric.load() == Metric::INODES;
    std::stable_sort(items.begin(), items.end(), [by_files](const CleanupItem& a, const CleanupItem& b) {
        return by_files ? a.inodes > b.inodes : a.disk > b.disk;
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

    if (kept.size() > 500)
        kept.resize(500);

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
    // Bytes compared as unsigned: with plain chars, every non-ASCII byte (é, 数, 🚀) failed to match
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
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

    walkAll([&](const std::string& path, const DirTree::Node& node, const DirTree&) {
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
                            node.complete.load(std::memory_order_acquire), true, node.error_below.load(std::memory_order_relaxed)};

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

Scanner::ScannerRemoveResult Scanner::remove(const fs::path& path, Progress* shown) {
    // What the progress window shows (a deletion of a single file needs none)
    Progress unseen;
    Progress& progress = shown ? *shown : unseen;

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
    const std::string parent_path = path.parent_path().string();

    // The tree that has the folder holding it: the main one, or the new scan of a folder above it
    const auto parent_ov = overlayFor(parent_path);
    DirTree& parent_tree = parent_ov ? *parent_ov->tree : tree;
    const uint32_t parent = parent_tree.find(parent_path);
    const int64_t scanned_from = parent_ov ? parent_ov->started_at : started_at.load();

    // Removing an entry takes writing in its folder (left as it is: it isn't part of what is removed)
    if (access(path.parent_path().c_str(), W_OK | X_OK) != 0) {
        return ScannerRemoveResult{"Nothing was deleted: " + scope.display(path.parent_path().string())
                                   + " is read-only (chmod u+w on it to delete what it holds).", true};
    }

    // A folder: everything in it must be removable, or nothing is removed (see Remover::check())
    size_t made_writable = 0;
    Remover::Check check;
    if (is_dir) {
        progress.step("Checking that all of it can be deleted");
        check = Remover::check(path.string(), scope.uid, progress);

        if (progress.stop.load())
            return ScannerRemoveResult{"stopped before anything was deleted", false};

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
    - A file changed since the scan started (or made after it) may have been counted with another size, or not
      at all: its folder is scanned again instead (`rescan` in the result), so the totals stay exact
    */
    DirTree::Counts file_counts;
    std::string rescan_after;
    if (!is_dir) {
        if (parent != DirTree::NONE)
            waitFor([&] { return parent_tree.node(parent).read.load(std::memory_order_acquire); });

        struct stat st;
        if (parent != DirTree::NONE && lstat(path.c_str(), &st) == 0 && st.st_nlink <= 1) {
            if (st.st_ctime >= scanned_from) {
                rescan_after = parent_path;
            } else {
                file_counts.bytes = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
                file_counts.disk = static_cast<uint64_t>(st.st_blocks) * 512;
                file_counts.inodes = 1;
            }
        }
    }

    if (is_dir) {
        // On several threads (see Remover): what the scan counted in it is what the bar goes to
        const Totals expected = totals(path);
        progress.step("Deleting the files, then the folders: " + std::to_string(Remover::threads()) + " folders at a time", expected.inodes);
        progress.disk = expected.disk;

        const Remover::Result removed = Remover::remove(path.string(), check.folders, progress);

        /* Still there: stopped, or a failure midway (everything was checked first: the disk changed meanwhile, an I/O error...)
        - Part of it is gone: its totals come from scanning what is left (`rescan`)
        */
        struct stat st;
        if (lstat(path.c_str(), &st) == 0) {
            const std::string count = FormatUtils::withSeparators(removed.removed) + " entries deleted, the rest is still there";
            if (progress.stop.load())
                return ScannerRemoveResult{"stopped: " + count, false, path.string()};
            return ScannerRemoveResult{(removed.error.empty() ? std::string("It couldn't all be deleted") : removed.error) + ". " + count + ".", true, path.string()};
        }
    } else {
        fs::remove(path, ec);
        if (ec)
            return ScannerRemoveResult{ec.message(), true};
    }

    if (!is_dir) {
        if (parent != DirTree::NONE && rescan_after.empty())
            adjust(parent_path, {}, file_counts);
    } else {
        /* A folder: everything the scan counted in it leaves every folder above it
        - Its totals are final once it is complete: during the scan that comes fast, its content being gone
        - Looked up after the removal: the scan may have just found it
        */
        const auto dir_ov = overlayFor(path.native());
        DirTree& dir_tree = dir_ov ? *dir_ov->tree : tree;
        const uint32_t dir = dir_tree.find(path.native());

        if (dir != DirTree::NONE) {
            waitFor([&] { return dir_tree.node(dir).complete.load(std::memory_order_acquire); });

            DirTree::Node& node = dir_tree.node(dir);
            const DirTree::Counts counts{node.size.load(), node.disk.load(), node.inodes.load(), 0};
            const bool had_errors = node.error_below.load();

            node.removed = true;
            adjust(parent_path, {}, counts);

            // Its unreadable folders are gone with it
            if (had_errors)
                refreshErrorMarks(parent_path);
        }
    }

    if (rescan_after.empty())
        this->snapshot();

    // Not an error: said next to the deletion
    std::string remark;
    if (made_writable > 0)
        remark = std::to_string(made_writable) + " read-only folder" + (made_writable > 1 ? "s" : "") + " in it made writable first";

    return ScannerRemoveResult{remark, false, rescan_after};
}

std::shared_ptr<Scanner::Overlay> Scanner::overlayFor(std::string_view p) const {
    if (overlay_count.load(std::memory_order_acquire) == 0)
        return nullptr;

    std::lock_guard<std::mutex> lock(overlays_mutex);
    std::shared_ptr<Overlay> best;
    for (const auto& ov : overlays) {
        if (UserScope::isUnder(p, ov->root) && (!best || ov->root.size() > best->root.size()))
            best = ov;
    }
    return best;
}

void Scanner::walkAll(const TreeFn& fn) {
    std::vector<std::shared_ptr<Overlay>> list;
    if (overlay_count.load(std::memory_order_acquire) > 0) {
        std::lock_guard<std::mutex> lock(overlays_mutex);
        list = overlays;
    }

    walkTree(tree, path.string(), list, fn);
}

void Scanner::walkTree(const DirTree& t, const std::string& root, const std::vector<std::shared_ptr<Overlay>>& list, const TreeFn& fn) {
    t.walk([&](const std::string& p, const DirTree::Node& node) {
        // A folder scanned again: its new tree instead of the old nodes
        if (!list.empty() && p != root) {
            for (const auto& ov : list) {
                if (ov->root == p) {
                    walkTree(*ov->tree, ov->root, list, fn);
                    return false;
                }
            }
        }
        return fn(p, node, t);
    });
}

void Scanner::adjust(std::string dir, const DirTree::Counts& added, const DirTree::Counts& removed) {
    while (!dir.empty()) {
        const auto ov = overlayFor(dir);
        DirTree& t = ov ? *ov->tree : tree;

        const uint32_t id = t.find(dir);
        if (id == DirTree::NONE)
            return;

        // Up to the root of that tree, in one walk
        t.add(id, added);
        t.subtract(id, removed);

        if (!ov)
            return;

        // Then above the folder that was scanned again, in the tree that holds it
        dir = parentOf(ov->root);
    }
}

void Scanner::refreshErrorMarks(std::string dir) {
    // Once a folder holds an error, so does every folder above it
    bool below = false;

    while (!dir.empty()) {
        const auto ov = overlayFor(dir);
        DirTree& t = ov ? *ov->tree : tree;

        const uint32_t id = t.find(dir);
        if (id == DirTree::NONE)
            return;

        DirTree::Node& node = t.node(id);
        if (!below) {
            below = node.error.load(std::memory_order_relaxed) != 0;
            if (const DirTree::Block* block = node.children.load(std::memory_order_acquire)) {
                for (uint32_t i = 0; i < block->count() && !below; i++)
                    below = totals(childOf(dir, block->name(i))).error;
            }
        }
        node.error_below.store(below, std::memory_order_relaxed);

        dir = parentOf(dir);
    }
}

Scanner::RescanResult Scanner::rescan(const std::string& target, Progress* progress) {
    if (stopping.load())
        return RescanResult{"", true};

    if (scope.relation(target) != UserScope::Relation::INSIDE)
        return RescanResult{"Only your spaces and what they hold can be scanned again.", true};

    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(target, ec)))
        return RescanResult{scope.display(target) + " isn't a folder anymore.", true};

    // What the main scan still adds below a folder it hasn't finished would be counted twice
    if (!done.load()) {
        std::string probe = target;
        bool finished = false;
        while (!probe.empty()) {
            const auto ov = overlayFor(probe);
            const DirTree& t = ov ? *ov->tree : tree;
            const uint32_t id = t.find(probe);
            if (id != DirTree::NONE) {
                finished = t.node(id).complete.load(std::memory_order_acquire);
                break;
            }
            probe = parentOf(probe);
        }

        if (!finished)
            return RescanResult{"The scan hasn't finished " + scope.display(target) + " yet: it shows up once it has.", true};
    }

    const Totals before = totals(target);

    auto ov = std::make_shared<Overlay>();
    ov->root = target;
    ov->tree = std::make_unique<DirTree>(target);
    ov->started_at = std::time(nullptr);

    // Shown: what is found so far, against what the previous scan found (stopped with the window, or on quit)
    const std::atomic<bool>& stop = progress ? progress->stop : stopping;
    if (progress)
        progress->step("Scanning " + scope.display(target) + " again", before.inodes);

    std::atomic<bool> walking{true};
    std::thread watch([&] {
        while (progress && walking.load()) {
            progress->done = ov->tree->node(DirTree::ROOT).inodes.load(std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    Walker walker(*ov->tree, scope, stop, scanThreads(), oldCutoff());
    walker.run();
    walking = false;
    watch.join();

    if (stop.load() || stopping.load())
        return RescanResult{stopping.load() ? "" : "stopped: " + scope.display(target) + " keeps its previous totals", stopping.load()};

    const DirTree::Counts now = countsOf(ov->tree->node(DirTree::ROOT));

    // From here on, this folder and everything below it come from the new tree (older scans inside it are replaced)
    {
        std::lock_guard<std::mutex> lock(overlays_mutex);
        overlays.erase(std::remove_if(overlays.begin(), overlays.end(), [&target](const std::shared_ptr<Overlay>& o) {
            return UserScope::isUnder(o->root, target);
        }), overlays.end());
        overlays.push_back(ov);
        overlay_count.store(overlays.size(), std::memory_order_release);
    }

    // The folders above get the difference
    auto delta = [](uint64_t after, uint64_t was, uint64_t& up, uint64_t& down) {
        if (after >= was) up = after - was; else down = was - after;
    };
    DirTree::Counts added, removed;
    delta(now.bytes, before.bytes, added.bytes, removed.bytes);
    delta(now.disk, before.disk, added.disk, removed.disk);
    delta(now.inodes, before.inodes, added.inodes, removed.inodes);
    added.newest = now.newest;
    adjust(parentOf(target), added, removed);

    // Unreadable folders: the new tree has its own marks, the ones above are worked out again
    if (before.error || ov->tree->node(DirTree::ROOT).error_below.load())
        refreshErrorMarks(parentOf(target));

    // Its largest files and core dumps, from the new scan
    {
        const Walker::Findings found = walker.findings();
        std::lock_guard<std::mutex> lock(findings_mutex);

        auto inside = [&target](const FoundFile& f) { return UserScope::isUnder(f.path, target); };
        largest.erase(std::remove_if(largest.begin(), largest.end(), inside), largest.end());
        old_files.erase(std::remove_if(old_files.begin(), old_files.end(), inside), old_files.end());
        cores.erase(std::remove_if(cores.begin(), cores.end(), inside), cores.end());

        for (const auto& hit : found.largest)
            largest.push_back(FoundFile{filePath(*ov->tree, hit.dir, hit.name), hit.disk, hit.bytes, hit.mtime});
        for (const auto& hit : found.old)
            old_files.push_back(FoundFile{filePath(*ov->tree, hit.dir, hit.name), hit.disk, hit.bytes, hit.mtime});
        for (const auto& hit : found.cores)
            cores.push_back(FoundFile{filePath(*ov->tree, hit.dir, hit.name), hit.disk, hit.bytes, hit.mtime});

        auto bigger = [](const FoundFile& a, const FoundFile& b) { return a.disk > b.disk; };
        std::sort(largest.begin(), largest.end(), bigger);
        if (largest.size() > Walker::LARGEST_FILES)
            largest.resize(Walker::LARGEST_FILES);
        std::sort(old_files.begin(), old_files.end(), bigger);
        if (old_files.size() > Walker::OLD_FILES)
            old_files.resize(Walker::OLD_FILES);
    }

    this->snapshot();

    // "~/x: 1.20 GiB on disk, 12.3k files (+300 files)"
    std::string change;
    if (now.inodes != before.inodes && before.known) {
        const bool more = now.inodes > before.inodes;
        change = std::string(" (") + (more ? "+" : "-") + FormatUtils::withSeparators(more ? now.inodes - before.inodes : before.inodes - now.inodes) + " files)";
    }

    return RescanResult{scope.display(target) + ": " + FormatUtils::toReadable(now.disk, " ") + " on disk, "
                        + FormatUtils::withSeparators(now.inodes) + " files" + change, false};
}

void Scanner::addFile(const std::string& file) {
    struct stat st;
    if (lstat(file.c_str(), &st) != 0)
        return;

    const DirTree::Counts counts{S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0,
                                 static_cast<uint64_t>(st.st_blocks) * 512, 1, st.st_mtime};
    adjust(parentOf(file), counts, {});
}

std::vector<Scanner::GrowthItem> Scanner::growth(size_t max_items) {
    std::vector<GrowthItem> items;
    if (!cache_ready.load(std::memory_order_acquire) || cache.empty())
        return items;

    const bool files = metric.load() == Metric::INODES;
    const int64_t least = files ? 1000 : static_cast<int64_t>(100 * MIB);

    auto previous = [this](const std::string& p) -> std::optional<int64_t> {
        const Cached* c = cached(p);
        if (!c) return std::nullopt;
        return static_cast<int64_t>(value(Totals{c->bytes, c->disk, c->inodes, c->newest, true, true}));
    };

    walkAll([&](const std::string& p, const DirTree::Node& node, const DirTree&) {
        const UserScope::Relation relation = scope.relation(p);
        if (relation != UserScope::Relation::INSIDE)
            return relation == UserScope::Relation::ANCESTOR;

        // Still growing: compared once scanned. A space the previous scan didn't cover can't be compared
        if (!node.complete.load(std::memory_order_acquire))
            return false;
        if (scope.isRoot(p) && !cached(p))
            return false;

        const Totals now{node.size.load(std::memory_order_relaxed), node.disk.load(std::memory_order_relaxed),
                         node.inodes.load(std::memory_order_relaxed), node.newest.load(std::memory_order_relaxed),
                         true, true, node.error_below.load(std::memory_order_relaxed)};
        const std::optional<int64_t> was = previous(p);
        const int64_t delta = static_cast<int64_t>(value(now)) - was.value_or(0);

        // Net growth: below a folder that didn't grow, nothing is looked at
        if (delta < least)
            return false;

        // Listed where it happened: not when one sub-folder holds most of it
        int64_t most = 0;
        if (const DirTree::Block* block = node.children.load(std::memory_order_acquire)) {
            for (uint32_t i = 0; i < block->count(); i++) {
                const std::string child = childOf(p, block->name(i));
                most = std::max(most, static_cast<int64_t>(value(totals(child))) - previous(child).value_or(0));
            }
        }

        if (static_cast<double>(most) < static_cast<double>(delta) * MOSTLY)
            items.push_back(GrowthItem{p, delta, now, !was});
        return true;
    });

    std::sort(items.begin(), items.end(), [](const GrowthItem& a, const GrowthItem& b) { return a.delta > b.delta; });
    if (items.size() > max_items)
        items.resize(max_items);

    return items;
}

std::vector<Scanner::ReadError> Scanner::readErrors(size_t max_items) {
    std::vector<ReadError> out;

    walkAll([&](const std::string& p, const DirTree::Node& node, const DirTree&) {
        if (!node.error_below.load(std::memory_order_relaxed))
            return false;

        const int16_t error = node.error.load(std::memory_order_relaxed);
        if (error != 0 && out.size() < max_items)
            out.push_back(ReadError{p, error});
        return out.size() < max_items;
    });

    return out;
}

uint32_t Scanner::readErrorCount() {
    // The main tree counts its own; once folders were scanned again, their old errors no longer count
    if (overlay_count.load(std::memory_order_acquire) == 0)
        return tree.errors();

    uint32_t count = 0;
    walkAll([&count](const std::string&, const DirTree::Node& node, const DirTree&) {
        if (!node.error_below.load(std::memory_order_relaxed))
            return false;
        if (node.error.load(std::memory_order_relaxed) != 0)
            count++;
        return true;
    });
    return count;
}

void Scanner::scan() {
    started_at.store(std::time(nullptr));

    // The previous scan is read while this one starts: it only delays what is shown, never the scan
    std::thread loader(&Scanner::loadCache, this);

    Walker walker(tree, scope, done, scanThreads(), oldCutoff());
    walker.run();

    {
        const Walker::Findings found = walker.findings();
        std::lock_guard<std::mutex> lock(findings_mutex);

        // A folder scanned again meanwhile already gave its own (see rescan())
        auto add = [this](std::vector<FoundFile>& list, const Walker::FileHit& hit) {
            std::string file = filePath(tree, hit.dir, hit.name);
            if (!overlayFor(file))
                list.push_back(FoundFile{std::move(file), hit.disk, hit.bytes, hit.mtime});
        };
        for (const auto& hit : found.largest)
            add(largest, hit);
        for (const auto& hit : found.old)
            add(old_files, hit);
        for (const auto& hit : found.cores)
            add(cores, hit);

        auto bigger = [](const FoundFile& a, const FoundFile& b) { return a.disk > b.disk; };
        std::sort(largest.begin(), largest.end(), bigger);
        if (largest.size() > Walker::LARGEST_FILES)
            largest.resize(Walker::LARGEST_FILES);
        std::sort(old_files.begin(), old_files.end(), bigger);
        if (old_files.size() > Walker::OLD_FILES)
            old_files.resize(Walker::OLD_FILES);
    }

    loader.join();
    this->done = true;
}

void Scanner::stop() {
    this->stopping = true;
    this->done = true;
}

bool Scanner::isDone() {
    return this->done;
}
