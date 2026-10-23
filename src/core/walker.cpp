#include "../../include/core/walker.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {

// What getdents64() writes, one after the other, each `reclen` bytes long
struct KernelDirent {
    uint64_t ino;
    int64_t off;
    unsigned short reclen;
    unsigned char type;
    char name[1];
};

constexpr unsigned char TYPE_UNKNOWN = 0; // DT_UNKNOWN: the filesystem doesn't tell, stat is needed
constexpr unsigned char TYPE_DIR = 4;     // DT_DIR
constexpr unsigned char TYPE_FILE = 8;    // DT_REG

constexpr size_t ENTRIES_BUFFER = 256 * 1024;

/* What the scan needs to know about an entry
- statx() with AT_STATX_DONT_SYNC (no metadata sync with the other nodes) was measured on ROMEO's GPFS:
  no gain over fstatat(), so the plain call stays.
*/
struct EntryStat {
    mode_t mode;
    uid_t uid;
    uint64_t size;
    uint64_t disk;   // st_blocks * 512
    int64_t mtime;
    ino_t ino;
    nlink_t links;
};

bool statEntry(int dir_fd, const char* name, EntryStat& out) {
    struct stat st;
    if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return false;

    out = EntryStat{st.st_mode, st.st_uid, static_cast<uint64_t>(st.st_size), static_cast<uint64_t>(st.st_blocks) * 512, st.st_mtime, st.st_ino, st.st_nlink};
    return true;
}

/* Thread count when the config says 0 (automatic)
- Two per core: on a disk that isn't cached yet most threads are waiting for it, not using the CPU.
- Capped, because past that point the threads mostly wait for each other inside the kernel.
- Measured on ROMEO (GPFS, 1.5M files): 16 threads 33 s, 32 threads 26 s, 64 threads 28 s, 128 threads 31 s.
*/
constexpr unsigned AUTO_THREADS_PER_CORE = 2;
constexpr unsigned MAX_AUTO_THREADS = 32;
constexpr unsigned MAX_THREADS = 256;

}

Walker::Walker(DirTree& _tree, const UserScope& _scope, const std::atomic<bool>& _stop, unsigned threads) : tree(_tree), scope(_scope), stop(_stop), seen(SEEN_BUCKETS) {
    if (threads == 0)
        threads = std::min(std::max(1u, std::thread::hardware_concurrency()) * AUTO_THREADS_PER_CORE, MAX_AUTO_THREADS);

    queues = std::vector<Queue>(std::min(threads, MAX_THREADS));
}

bool Walker::isSkippedFs(int fd) {
    struct statfs stfs;
    if (fstatfs(fd, &stfs) != 0)
        return true;

    switch (stfs.f_type) {
        // Virtual filesystems
        case PROC_SUPER_MAGIC:
        case SYSFS_MAGIC:
        // Network shares
        case NFS_SUPER_MAGIC:
        case SMB_SUPER_MAGIC:
            return true;
        default:
            return false;
    }
}

bool Walker::firstVisit(dev_t dev, ino_t ino) {
    const Inode inode{dev, ino};
    SeenBucket& bucket = seen[InodeHash()(inode) % SEEN_BUCKETS];

    std::lock_guard<std::mutex> lock(bucket.mutex);
    return bucket.inodes.insert(inode).second;
}

void Walker::run() {
    // The root has no parent: (dev_t)-1 is never a real device, so its filesystem is always checked
    pending.store(1);
    queues[0].tasks.push_back(Task{DirTree::ROOT, static_cast<dev_t>(-1)});

    // If the system refuses more threads, the ones we got (at least this one) do the whole scan
    std::vector<std::thread> threads;
    try {
        for (unsigned i = 1; i < queues.size(); i++)
            threads.emplace_back(&Walker::work, this, i);
    } catch (const std::system_error&) {}

    work(0);

    for (auto& thread : threads)
        thread.join();
}

bool Walker::pop(unsigned self, Task& task) {
    Queue& queue = queues[self];
    std::lock_guard<std::mutex> lock(queue.mutex);

    if (queue.tasks.empty())
        return false;

    task = queue.tasks.back();
    queue.tasks.pop_back();
    return true;
}

// Takes the older half of another thread's tasks: runs one, keeps the rest.
bool Walker::steal(unsigned self, Task& task, Scratch& scratch) {
    for (size_t i = 1; i < queues.size(); i++) {
        Queue& victim = queues[(self + i) % queues.size()];

        {
            std::lock_guard<std::mutex> lock(victim.mutex);
            if (victim.tasks.empty())
                continue;

            const size_t taken = (victim.tasks.size() + 1) / 2;
            scratch.stolen.assign(victim.tasks.begin(), victim.tasks.begin() + taken);
            victim.tasks.erase(victim.tasks.begin(), victim.tasks.begin() + taken);
        }

        task = scratch.stolen.front();

        if (scratch.stolen.size() > 1) {
            std::lock_guard<std::mutex> lock(queues[self].mutex);
            queues[self].tasks.insert(queues[self].tasks.end(), scratch.stolen.rbegin(), scratch.stolen.rend() - 1);
        }

        return true;
    }

    return false;
}

void Walker::work(unsigned self) {
    Scratch scratch;
    scratch.entries.resize(ENTRIES_BUFFER);

    unsigned idle = 0;

    while (true) {
        Task task;

        if (pop(self, task) || steal(self, task, scratch)) {
            idle = 0;

            // Once stopped, tasks are only drained so every thread sees `pending` reach zero
            if (!stop.load(std::memory_order_relaxed))
                visit(task, self, scratch);

            pending.fetch_sub(1);
            continue;
        }

        if (pending.load() == 0)
            break;

        // Nothing to steal right now, but another thread is still listing a directory
        if (++idle < 32)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    std::lock_guard<std::mutex> lock(findings_mutex);
    merged.cores.insert(merged.cores.end(), scratch.cores.begin(), scratch.cores.end());
    merged.largest.insert(merged.largest.end(), scratch.largest.begin(), scratch.largest.end());
}

Walker::Findings Walker::findings() {
    std::lock_guard<std::mutex> lock(findings_mutex);
    Findings out = merged;

    auto bigger = [](const FileHit& a, const FileHit& b) { return a.disk > b.disk; };
    std::sort(out.largest.begin(), out.largest.end(), bigger);
    if (out.largest.size() > LARGEST_FILES)
        out.largest.resize(LARGEST_FILES);

    std::sort(out.cores.begin(), out.cores.end(), bigger);
    return out;
}

void Walker::remember(Scratch& scratch, uint32_t dir, const char* name, uint64_t disk, uint64_t bytes, int64_t mtime) {
    // Core dumps: "core" or "core.<pid>"
    if (std::strncmp(name, "core", 4) == 0 && (name[4] == '\0' || (name[4] == '.' && name[5] != '\0' && std::strspn(name + 5, "0123456789") == std::strlen(name + 5))))
        scratch.cores.push_back(FileHit{disk, bytes, dir, name, mtime});

    // The smallest of the kept files is at the front of the heap
    auto bigger = [](const FileHit& a, const FileHit& b) { return a.disk > b.disk; };

    if (scratch.largest.size() < LARGEST_FILES) {
        scratch.largest.push_back(FileHit{disk, bytes, dir, name, mtime});
        std::push_heap(scratch.largest.begin(), scratch.largest.end(), bigger);
    } else if (disk > scratch.largest.front().disk) {
        std::pop_heap(scratch.largest.begin(), scratch.largest.end(), bigger);
        scratch.largest.back() = FileHit{disk, bytes, dir, name, mtime};
        std::push_heap(scratch.largest.begin(), scratch.largest.end(), bigger);
    }
}

void Walker::visit(const Task& task, unsigned self, Scratch& scratch) {
    tree.path(task.dir, scratch.path);

    // Above the user's spaces nothing is counted, only the way down to them is listed
    const UserScope::Relation relation = scope.relation(scratch.path);
    if (relation == UserScope::Relation::OUTSIDE) {
        tree.listed(task.dir, 0);
        return;
    }

    const bool above_scope = relation == UserScope::Relation::ANCESTOR;

    const int fd = open(scratch.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        tree.listed(task.dir, 0);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0
        || (st.st_dev != task.parent_dev && isSkippedFs(fd))
        || !firstVisit(st.st_dev, st.st_ino)) {
        close(fd);
        tree.listed(task.dir, 0);
        return;
    }

    // The directory itself uses an inode and some blocks too
    DirTree::Counts counts;
    if (!above_scope && st.st_uid == scope.uid) {
        counts.inodes = 1;
        counts.disk = static_cast<uint64_t>(st.st_blocks) * 512;
        counts.newest = st.st_mtime;
    }

    uint32_t dir_count = 0;
    scratch.names.clear();

    // A huge directory takes many rounds, so the stop flag is checked on each of them
    while (!stop.load(std::memory_order_relaxed)) {
        const long length = syscall(SYS_getdents64, fd, scratch.entries.data(), scratch.entries.size());

        if (length < 0 && errno == EINTR)
            continue;
        if (length <= 0)
            break;

        for (long at = 0; at < length;) {
            const auto* entry = reinterpret_cast<const KernelDirent*>(scratch.entries.data() + at);
            const char* name = scratch.entries.data() + at + offsetof(KernelDirent, name);
            at += entry->reclen;

            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
                continue;

            unsigned char type = entry->type;

            /* Everything but directories needs a stat: size, blocks, owner, date
            - Directories count themselves when they are visited
            - Above the user's spaces only directories matter: no stat unless the type is unknown
            */
            if (type != TYPE_DIR) {
                if (above_scope && type != TYPE_UNKNOWN)
                    continue;

                EntryStat file;
                if (!statEntry(fd, name, file))
                    continue;

                if (!S_ISDIR(file.mode)) {
                    /* Hard links (conda environments, git, spack...): one file in several folders
                    - The quotas count it once; so does the scan, in the first folder it is seen in
                    */
                    if (file.links > 1 && !firstVisit(st.st_dev, file.ino))
                        continue;

                    if (!above_scope && file.uid == scope.uid) {
                        counts.inodes++;
                        counts.disk += file.disk;
                        counts.newest = std::max(counts.newest, file.mtime);

                        if (S_ISREG(file.mode)) {
                            counts.bytes += file.size;
                            remember(scratch, task.dir, name, file.disk, file.size, file.mtime);
                        }
                    }
                    continue;
                }

                type = TYPE_DIR;
            }

            if (above_scope) {
                scratch.child.assign(scratch.path == "/" ? "" : scratch.path);
                scratch.child += '/';
                scratch.child += name;

                if (scope.relation(scratch.child) == UserScope::Relation::OUTSIDE)
                    continue;
            }

            scratch.names.insert(scratch.names.end(), name, name + std::strlen(name) + 1);
            dir_count++;
        }
    }

    close(fd);

    // Interrupted: the listing is partial, so this directory must never look complete (it would be cached so)
    if (stop.load(std::memory_order_relaxed))
        return;

    // Counted before the directory can be complete: a complete directory has its final totals
    tree.add(task.dir, counts);

    // Publish the sub-directories, then queue them
    const DirTree::Block* block = tree.attach(task.dir, scratch.names, dir_count);
    if (!block) {
        tree.listed(task.dir, 0);
        return;
    }

    tree.listed(task.dir, dir_count);
    pending.fetch_add(dir_count);

    std::lock_guard<std::mutex> lock(queues[self].mutex);
    for (uint32_t i = dir_count; i-- > 0;)
        queues[self].tasks.push_back(Task{block->first + i, st.st_dev});
}
