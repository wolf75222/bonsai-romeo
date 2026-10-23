#include "../../include/core/remover.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {

/* Threads
- Deleting changes the folders on GPFS, more work for it than reading them: fewer threads than the scan
*/
unsigned threadCount() {
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    return std::min(16u, std::max(2u, cores));
}

bool isDots(const char* name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

// fn(thread) on `count` threads, waited for; the calling thread is one of them (the only one if no other starts)
void onThreads(unsigned count, const std::function<void(unsigned)>& fn) {
    std::vector<std::thread> threads;
    try {
        for (unsigned i = 1; i < count; i++)
            threads.emplace_back(fn, i);
    } catch (const std::system_error&) {}

    fn(0);
    for (auto& thread : threads)
        thread.join();
}

/* One folder of check()
- Removing an entry takes writing in, and entering, the folder holding it
- The user's own folders without permission can be fixed (listed in `readonly`); someone else's can't: blocked
- In a sticky folder (like /tmp) only one's own entries can be removed
- Only folders are looked at (one readdir each); files need a stat only in sticky folders
*/
std::string checkFolder(const Remover::Folder& folder, uid_t uid, std::vector<std::string>& readonly, std::vector<Remover::Folder>& below) {
    const std::string& dir = folder.first;

    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return "";

    const bool usable = (st.st_mode & S_IRWXU) == S_IRWXU;
    if (st.st_uid == uid) {
        if (!usable) readonly.push_back(dir);
    } else if (access(dir.c_str(), R_OK | W_OK | X_OK) != 0) {
        return dir;
    }

    // An unreadable folder of the user's can only be listed once fixed: it is taken as it comes
    DIR* listing = opendir(dir.c_str());
    if (!listing)
        return "";

    const bool sticky = (st.st_mode & S_ISVTX) && st.st_uid != uid;
    std::string blocked;

    while (const dirent* entry = readdir(listing)) {
        if (isDots(entry->d_name))
            continue;

        std::string child = dir + "/" + entry->d_name;
        unsigned char type = entry->d_type;

        if (type == DT_UNKNOWN || sticky) {
            struct stat cs;
            if (lstat(child.c_str(), &cs) != 0)
                continue;
            if (sticky && cs.st_uid != uid) {
                blocked = child;
                break;
            }
            type = S_ISDIR(cs.st_mode) ? DT_DIR : DT_REG;
        }

        if (type == DT_DIR)
            below.emplace_back(std::move(child), folder.second + 1);
    }

    closedir(listing);
    return blocked;
}

// What the threads of remove() share
struct Shared {
    Progress& progress;
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> removed{0};
    std::mutex mutex;
    std::string error;

    explicit Shared(Progress& p) : progress(p) {}

    void fail(const std::string& path, int error_number) {
        std::lock_guard<std::mutex> lock(mutex);
        if (error.empty())
            error = path + ": " + std::strerror(error_number);
        failed = true;
    }

    bool halted() const {
        return failed.load() || progress.stop.load();
    }

    void count(unsigned slot, const std::string& path, bool show) {
        removed++;
        progress.done++;
        if (show)
            progress.note(slot, path);
    }
};

// Deletes what `dir` holds but its folders: the names are read first (deleting while reading may skip some)
void emptyFolder(const std::string& dir, Shared& shared, unsigned slot) {
    const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT)
            shared.fail(dir, errno);
        return;
    }

    DIR* listing = fdopendir(fd);
    if (!listing) {
        shared.fail(dir, errno);
        close(fd);
        return;
    }

    std::vector<std::string> files;
    while (const dirent* entry = readdir(listing)) {
        if (isDots(entry->d_name))
            continue;

        bool folder = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN) {
            struct stat st;
            folder = fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
        }
        if (!folder)
            files.emplace_back(entry->d_name);
    }

    uint64_t n = 0;
    for (const auto& name : files) {
        if (shared.halted())
            break;

        if (unlinkat(fd, name.c_str(), 0) != 0) {
            if (errno == ENOENT)
                continue;
            shared.fail(dir + "/" + name, errno);
            break;
        }

        shared.count(slot, dir + "/" + name, (n++ & 15) == 0);
    }

    closedir(listing);
}

}

unsigned Remover::threads() {
    return threadCount();
}

Remover::Check Remover::check(const std::string& root, uid_t uid, Progress& progress) {
    Check out;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Folder> queue{{root, 0}};
    unsigned busy = 0;   // Threads on a folder, whose sub-folders aren't queued yet

    onThreads(threadCount(), [&](unsigned) {
        std::vector<Folder> mine, below;
        std::vector<std::string> readonly;

        while (true) {
            Folder folder;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return !queue.empty() || busy == 0 || !out.blocked.empty() || progress.stop.load(); });

                // Nothing queued and nobody busy: every folder was checked
                if (!out.blocked.empty() || progress.stop.load() || queue.empty())
                    break;

                folder = std::move(queue.back());
                queue.pop_back();
                busy++;
            }

            const std::string blocked = checkFolder(folder, uid, readonly, below);
            progress.checked++;

            {
                std::lock_guard<std::mutex> lock(mutex);
                busy--;
                mine.push_back(std::move(folder));
                if (!blocked.empty() && out.blocked.empty())
                    out.blocked = blocked;
                for (auto& f : below)
                    queue.push_back(std::move(f));
                below.clear();
            }
            cv.notify_all();
        }

        std::lock_guard<std::mutex> lock(mutex);
        std::move(mine.begin(), mine.end(), std::back_inserter(out.folders));
        std::move(readonly.begin(), readonly.end(), std::back_inserter(out.readonly));
        cv.notify_all();
    });

    return out;
}

Remover::Result Remover::remove(const std::string& root, const std::vector<Folder>& folders, Progress& progress) {
    Shared shared(progress);
    const unsigned threads = threadCount();

    // 1. The files: each thread takes the next folder
    std::atomic<size_t> next{0};
    onThreads(threads, [&](unsigned slot) {
        for (size_t i = next++; i < folders.size() && !shared.halted(); i = next++)
            emptyFolder(folders[i].first, shared, slot);
    });

    // 2. The folders, deepest first: a level is gone before the one above it (the root is the last one)
    std::map<uint32_t, std::vector<const std::string*>, std::greater<uint32_t>> levels;
    for (const auto& folder : folders)
        levels[folder.second].push_back(&folder.first);
    if (levels.empty())
        levels[0].push_back(&root);

    for (const auto& [depth, level] : levels) {
        if (shared.halted())
            break;

        std::atomic<size_t> at{0};
        onThreads(std::min<unsigned>(threads, static_cast<unsigned>(level.size())), [&](unsigned slot) {
            for (size_t i = at++; i < level.size() && !shared.halted(); i = at++) {
                const std::string& dir = *level[i];
                if (rmdir(dir.c_str()) == 0) {
                    shared.count(slot, dir, true);
                    continue;
                }

                int error = errno;

                // Something was written in it meanwhile (a job running): emptied again, once
                if (error == ENOTEMPTY || error == EEXIST) {
                    emptyFolder(dir, shared, slot);
                    if (rmdir(dir.c_str()) == 0) {
                        shared.count(slot, dir, true);
                        continue;
                    }
                    error = errno;
                }

                if (error == ENOENT)
                    continue;

                shared.fail(dir, error);
                break;
            }
        });
    }

    return Result{shared.removed.load(), shared.error};
}
