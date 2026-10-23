#include "../../include/core/archive.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

// Entries below `dir` (files, folders, links...) and `dir` itself: what `tar -t` lists for it
uint64_t countEntries(const std::string& dir, const std::atomic<bool>& stop) {
    uint64_t count = 1;
    std::vector<std::string> stack{dir};

    while (!stack.empty() && !stop.load()) {
        const std::string current = std::move(stack.back());
        stack.pop_back();

        // Unreadable: tar fails on it too, and nothing is deleted
        DIR* listing = opendir(current.c_str());
        if (!listing)
            continue;

        while (const dirent* entry = readdir(listing)) {
            const char* name = entry->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
                continue;

            count++;
            std::string child = current + "/" + name;
            unsigned char type = entry->d_type;

            if (type == DT_UNKNOWN) {
                struct stat st;
                if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                    type = DT_DIR;
            }
            if (type == DT_DIR)
                stack.push_back(std::move(child));
        }

        closedir(listing);
    }

    return count;
}

/* Runs tar with `args`
- Its output is read as it comes: on_line() for each line of stdout (one per entry with -v or -t),
  the first line of stderr kept for the error message. Both pipes are read together, so tar never waits on one.
- Returns tar's exit status, or -1 when it couldn't start, was killed or was stopped
*/
int runTar(const std::vector<std::string>& args, const std::function<void()>& on_line, std::string& error,
           std::atomic<pid_t>& child, const std::atomic<bool>& stop) {
    int out[2], err[2];
    if (pipe2(out, O_CLOEXEC) != 0) {
        error = std::strerror(errno);
        return -1;
    }
    if (pipe2(err, O_CLOEXEC) != 0) {
        error = std::strerror(errno);
        close(out[0]);
        close(out[1]);
        return -1;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("tar"));
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, "tar", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    close(err[1]);

    if (spawned != 0) {
        error = std::string("tar couldn't be started: ") + std::strerror(spawned);
        close(out[0]);
        close(err[0]);
        return -1;
    }
    child = pid;

    std::string messages;
    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    char buffer[65536];

    for (int open = 2; open > 0;) {
        if (poll(fds, 2, 500) < 0 && errno != EINTR)
            break;

        for (auto& fd : fds) {
            if (fd.fd < 0 || !(fd.revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            const ssize_t n = read(fd.fd, buffer, sizeof(buffer));
            if (n <= 0) {
                close(fd.fd);
                fd.fd = -1;
                open--;
                continue;
            }

            if (&fd == &fds[0]) {
                for (ssize_t i = 0; i < n; i++)
                    if (buffer[i] == '\n') on_line();
            } else if (messages.size() < 4096) {
                messages.append(buffer, static_cast<size_t>(n));
            }
        }
    }

    for (auto& fd : fds)
        if (fd.fd >= 0) close(fd.fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    child = 0;

    error = messages.substr(0, messages.find('\n'));
    if (stop.load() || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

}

std::string Archive::archiveName(const std::string& dir) {
    struct stat st;
    std::string name = dir + ".tar";
    for (int i = 1; lstat(name.c_str(), &st) == 0 || lstat((name + ".part").c_str(), &st) == 0; i++)
        name = dir + "." + std::to_string(i) + ".tar";
    return name;
}

Archive::Result Archive::pack(const std::string& dir, const std::function<void(uint64_t, uint64_t)>& progress,
                              std::atomic<pid_t>& child, const std::atomic<bool>& stop) {
    Result result;

    const size_t slash = dir.rfind('/');
    if (slash == std::string::npos || slash + 1 >= dir.size()) {
        result.error = "Not a folder that can be packed.";
        return result;
    }
    const std::string parent = slash == 0 ? "/" : dir.substr(0, slash);
    const std::string name = dir.substr(slash + 1);

    result.entries = countEntries(dir, stop);
    if (stop.load()) {
        result.error = "Stopped.";
        return result;
    }

    const std::string archive = archiveName(dir);
    const std::string part = archive + ".part";

    // The archive holds "name/...": it unpacks where the folder was (tar -xf name.tar)
    uint64_t done = 0;
    std::string error;
    const int created = runTar({"-cvf", part, "-C", parent, "--", name}, [&] {
        if ((++done & 1023) == 0)
            progress(done, result.entries);
    }, error, child, stop);

    if (created != 0) {
        unlink(part.c_str());
        result.error = stop.load() ? "Stopped." : "tar failed" + (error.empty() ? std::string(".") : ": " + error);
        return result;
    }
    progress(result.entries, result.entries);

    // Read back: every entry of the folder must be in it
    uint64_t listed = 0;
    const int read_back = runTar({"-tf", part}, [&] { listed++; }, error, child, stop);

    if (read_back != 0 || listed != result.entries) {
        unlink(part.c_str());
        if (stop.load())
            result.error = "Stopped.";
        else if (read_back != 0)
            result.error = "The archive can't be read back" + (error.empty() ? std::string(".") : ": " + error);
        else
            result.error = "The archive lists " + std::to_string(listed) + " entries, the folder holds " + std::to_string(result.entries)
                         + ": something changed in it meanwhile, or it holds what tar skips (sockets).";
        return result;
    }

    if (rename(part.c_str(), archive.c_str()) != 0) {
        result.error = std::string("The archive couldn't be renamed: ") + std::strerror(errno);
        unlink(part.c_str());
        return result;
    }

    result.ok = true;
    result.archive = archive;
    return result;
}
