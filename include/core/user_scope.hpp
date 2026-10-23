/* USER SCOPE
Explanation:
- On a shared cluster (ROMEO), most of what a disk analyzer sees belongs to other people.
- The scope is the part of the disk that belongs to the current user:
    - Their spaces: home, scratch, and /project/<group> for each project group they are in.
    - Inside these spaces, only files they own are counted and shown.
- Every space is stored by its real path (/project -> /gpfs/projet), so it matches what the scan sees.
- A path is either:
    - INSIDE   one of the spaces (or the space itself)
    - ANCESTOR of a space (/gpfs, /gpfs/home): only the way down to the spaces is kept
    - OUTSIDE  everything else, never scanned
- Spaces can be overridden with BONSAI_ROOTS="path1:path2:..."
*/

#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <grp.h>
#include <pwd.h>
#include <string>
#include <string_view>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

class UserScope {
public:
    enum class Relation { OUTSIDE, ANCESTOR, INSIDE };

    uid_t uid = 0;
    std::vector<std::string> roots; // Real paths, without trailing slash

    // How the user knows each space: (real path, shown as), e.g. ("/gpfs/home/bob", "~")
    std::vector<std::pair<std::string, std::string>> aliases;

    static UserScope detect() {
        UserScope scope;
        scope.uid = getuid();

        std::vector<fs::path> candidates;

        // Shown as "~" in paths, whatever the spaces come from
        std::string home;
        if (const passwd* pw = getpwuid(scope.uid); pw && pw->pw_dir)
            home = fs::weakly_canonical(pw->pw_dir).string();

        if (const char* env = std::getenv("BONSAI_ROOTS"); env && *env) {
            std::string list(env);
            size_t start = 0;

            while (start <= list.size()) {
                size_t end = list.find(':', start);
                if (end == std::string::npos) end = list.size();
                if (end > start) candidates.push_back(list.substr(start, end - start));
                start = end + 1;
            }
        } else {
            const passwd* pw = getpwuid(scope.uid);
            const std::string user = pw ? pw->pw_name : "";

            if (pw && pw->pw_dir)
                candidates.push_back(pw->pw_dir);

            if (!user.empty()) {
                candidates.push_back(fs::path("/scratch_p") / user);
                candidates.push_back(fs::path("/scratch") / user);
            }

            // One project directory per group the user is in
            int count = getgroups(0, nullptr);
            std::vector<gid_t> groups(count > 0 ? count : 0);
            if (count > 0) count = getgroups(count, groups.data());

            for (int i = 0; i < count; i++) {
                if (const group* gr = getgrgid(groups[i]); gr && gr->gr_name)
                    candidates.push_back(fs::path("/project") / gr->gr_name);
            }
        }

        for (const auto& candidate : candidates) {
            std::error_code ec;
            if (!fs::is_directory(candidate, ec)) continue;

            std::string real = fs::canonical(candidate, ec).string();
            if (ec) continue;

            scope.add(real);
            scope.aliases.emplace_back(real, real == home ? "~" : candidate.string());
        }

        return scope;
    }

    bool empty() const { return roots.empty(); }

    Relation relation(std::string_view path) const {
        while (path.size() > 1 && path.back() == '/')
            path.remove_suffix(1);

        Relation result = Relation::OUTSIDE;

        for (const auto& root : roots) {
            if (isUnder(path, root))
                return Relation::INSIDE;

            if (isUnder(root, path))
                result = Relation::ANCESTOR;
        }

        return result;
    }

    // Deepest directory that contains every space: where the app starts by default
    fs::path commonAncestor() const {
        if (roots.empty()) return "/";

        std::string common = roots.front();
        for (const auto& root : roots) {
            while (!isUnder(root, common)) {
                size_t slash = common.rfind('/');
                common = slash == 0 || slash == std::string::npos ? "/" : common.substr(0, slash);
            }
        }

        return common;
    }

    // A real path the way the user knows it: /gpfs/home/bob/x -> ~/x, /gpfs/projet/r1/x -> /project/r1/x
    std::string display(const std::string& real) const {
        for (const auto& [path, shown] : aliases) {
            if (isUnder(real, path))
                return shown + real.substr(path.size());
        }
        return real;
    }

    bool isRoot(const std::string& real) const {
        return std::find(roots.begin(), roots.end(), real) != roots.end();
    }

    /* Short name of a space, for the pie: "home", "scratch", "r250127" (empty if `real` isn't a space)
    - label() adds what it is for the sidebar: "home", "scratch", "project r250127"
    */
    std::string shortName(const std::string& real) const {
        if (!isRoot(real))
            return "";

        const std::string shown = display(real);
        if (shown == "~") return "home";
        if (shown.rfind("/scratch", 0) == 0) return "scratch";
        if (shown.rfind("/project/", 0) == 0) return shown.substr(9);
        return fs::path(real).filename().string();
    }

    std::string label(const std::string& real) const {
        const std::string name = shortName(real);
        return display(real).rfind("/project/", 0) == 0 ? "project " + name : name;
    }

    bool owns(const fs::path& path) const {
        struct stat st;
        return lstat(path.c_str(), &st) == 0 && st.st_uid == uid;
    }

    // True if `path` is `base` or something below it
    static bool isUnder(std::string_view path, std::string_view base) {
        if (base == "/") return !path.empty() && path.front() == '/';
        if (path.size() < base.size() || path.compare(0, base.size(), base) != 0) return false;
        return path.size() == base.size() || path[base.size()] == '/';
    }

private:
    void add(const std::string& real) {
        // A space inside another one is already covered
        for (const auto& root : roots)
            if (isUnder(real, root)) return;

        std::vector<std::string> kept;
        for (auto& root : roots)
            if (!isUnder(root, real)) kept.push_back(std::move(root));

        kept.push_back(real);
        roots = std::move(kept);
    }
};
