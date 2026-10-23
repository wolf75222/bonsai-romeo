#include "../../include/core/quota.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <grp.h>
#include <map>
#include <pwd.h>
#include <regex>
#include <sstream>
#include <unistd.h>

namespace {

constexpr const char* DEVICE = "gpfs";
constexpr int QUERY_TIMEOUT = 10;       // Seconds per mmlsquota call
constexpr double NOTICE_RATIO = 0.85;   // Share of the soft limit that gets a notice

// Project groups, as in myquotas.sh
const std::regex PROJECT_GROUP("^r[0-9]{2,}$");

// Runs a command and returns its output ("" if it failed)
std::string run(const std::string& command) {
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return out;

    std::array<char, 4096> buffer;
    while (size_t n = fread(buffer.data(), 1, buffer.size(), pipe))
        out.append(buffer.data(), n);

    pclose(pipe);
    return out;
}

std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;

    while (std::getline(ss, field, sep))
        fields.push_back(field);

    return fields;
}

uint64_t number(const std::string& s) {
    try { return std::stoull(s); } catch (...) { return 0; }
}

/* mmlsquota -Y output
- One HEADER line naming the columns, then one line per fileset:
  mmlsquota:user:HEADER:...:blockUsage:blockQuota:blockLimit:...:filesUsage:...:filesetname:
  mmlsquota:user:0:1:::gpfs:USR:100267:-:25387696:104857600:110100480:0:none:549748:...:home:
- Block values are in KiB
*/
std::vector<std::map<std::string, std::string>> query(const std::string& args) {
    const std::string command = "timeout " + std::to_string(QUERY_TIMEOUT) + " mmlsquota -Y " + args + " " + DEVICE + " 2>/dev/null"
                              + " || timeout " + std::to_string(QUERY_TIMEOUT) + " /usr/lpp/mmfs/bin/mmlsquota -Y " + args + " " + DEVICE + " 2>/dev/null";

    std::vector<std::map<std::string, std::string>> rows;
    std::vector<std::string> header;

    std::stringstream ss(run(command));
    std::string line;

    while (std::getline(ss, line)) {
        auto fields = split(line, ':');
        if (fields.size() < 3 || fields[0] != "mmlsquota") continue;

        if (fields[2] == "HEADER") {
            header = fields;
            continue;
        }

        std::map<std::string, std::string> row;
        for (size_t i = 0; i < fields.size() && i < header.size(); i++)
            row[header[i]] = fields[i];

        rows.push_back(std::move(row));
    }

    return rows;
}

Quota::Row toRow(const std::map<std::string, std::string>& r, bool group, const std::string& name) {
    auto get = [&r](const char* key) {
        auto it = r.find(key);
        return it == r.end() ? std::string() : it->second;
    };

    Quota::Row row;
    row.group = group;
    row.name = name;
    row.fileset = get("filesetname");

    row.space.used = number(get("blockUsage")) * 1024;
    row.space.soft = number(get("blockQuota")) * 1024;
    row.space.hard = number(get("blockLimit")) * 1024;
    row.space.grace = get("blockGrace");

    row.inodes.used = number(get("filesUsage"));
    row.inodes.soft = number(get("filesQuota"));
    row.inodes.hard = number(get("filesLimit"));
    row.inodes.grace = get("filesGrace");

    if (row.fileset == "home") row.path = "/home/" + name;
    else if (row.fileset == "scratch") row.path = "/scratch_p/" + name;
    else if (row.fileset == "projet") row.path = group ? "/project/" + name : "/project";
    else row.path = "/" + row.fileset;

    return row;
}

}

Quota::State Quota::Limit::state() const {
    if (!limited()) return State::NO_LIMIT;
    if (hard != 0 && used >= hard) return State::HARD;
    if (grace == "expired") return State::EXPIRED;
    if (soft != 0 && used > soft) return State::GRACE;
    if (soft != 0 && used >= soft * NOTICE_RATIO) return State::NOTICE;
    return State::OK;
}

double Quota::Limit::ratio() const {
    const uint64_t max = hard != 0 ? hard : soft;
    if (max == 0) return 0;
    return std::min(1.0, static_cast<double>(used) / static_cast<double>(max));
}

Quota::State Quota::Row::state() const {
    return std::max(space.state(), inodes.state());
}

Quota::State Quota::Report::worst() const {
    State worst = State::OK;
    for (const auto& row : rows)
        worst = std::max(worst, row.state());
    return worst;
}

int Quota::Report::alerts() const {
    int count = 0;
    for (const auto& row : rows)
        if (row.state() >= State::GRACE) count++;
    return count;
}

const char* Quota::stateName(State s) {
    switch (s) {
        case State::OK:       return "ok";
        case State::NO_LIMIT: return "no individual limit";
        case State::NOTICE:   return "close to the soft limit";
        case State::GRACE:    return "soft limit exceeded, grace period running";
        case State::EXPIRED:  return "grace period over, writing blocked";
        case State::HARD:     return "hard limit reached, writing impossible";
    }
    return "";
}

Quota::Report Quota::fetch() {
    Report report;
    report.fetched_at = std::chrono::system_clock::now();

    const passwd* pw = getpwuid(getuid());
    const std::string user = pw ? pw->pw_name : "";

    // User quotas: home, scratch, projet
    for (const auto& r : query("")) {
        auto row = toRow(r, false, user);
        if (row.fileset == "home" || row.fileset == "scratch" || row.fileset == "projet")
            report.rows.push_back(std::move(row));
    }

    // Group quotas of every project the user is in
    int count = getgroups(0, nullptr);
    std::vector<gid_t> groups(count > 0 ? count : 0);
    if (count > 0) count = getgroups(count, groups.data());

    for (int i = 0; i < count; i++) {
        const group* gr = getgrgid(groups[i]);
        if (!gr || !gr->gr_name || !std::regex_match(gr->gr_name, PROJECT_GROUP)) continue;

        const std::string name = gr->gr_name;
        if (std::find(report.project_groups.begin(), report.project_groups.end(), name) != report.project_groups.end()) continue;
        report.project_groups.push_back(name);

        for (const auto& r : query("-g " + name)) {
            auto row = toRow(r, true, name);
            if (row.fileset == "projet")
                report.rows.push_back(std::move(row));
        }
    }

    /* Stray data
    - The personal group (primary group, no declared members, not a project) has no quota on projet.
    - Anything it still owns there isn't counted on the project and can block writes.
    */
    if (pw && !report.project_groups.empty()) {
        const group* pg = getgrgid(pw->pw_gid);
        const bool shared = pg && pg->gr_mem && pg->gr_mem[0];

        if (pg && pg->gr_name && !shared && !std::regex_match(pg->gr_name, PROJECT_GROUP)) {
            for (const auto& r : query(std::string("-g ") + pg->gr_name)) {
                auto row = toRow(r, true, pg->gr_name);
                if (row.fileset == "projet" && (row.space.used > 0 || row.inodes.used > 0)) {
                    report.stray_group = pg->gr_name;
                    report.stray_bytes = row.space.used;
                    report.stray_inodes = row.inodes.used;
                }
            }
        }
    }

    if (report.rows.empty())
        report.error = "No quota could be read (mmlsquota missing, or GPFS not answering).";

    return report;
}

QuotaMonitor::QuotaMonitor() : thread(&QuotaMonitor::worker, this) {}

QuotaMonitor::~QuotaMonitor() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
    }
    cv.notify_all();
    thread.join();
}

Quota::Report QuotaMonitor::get() {
    std::lock_guard<std::mutex> lock(mutex);
    return report;
}

bool QuotaMonitor::ready() {
    std::lock_guard<std::mutex> lock(mutex);
    return has_report;
}

void QuotaMonitor::refresh() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        want_refresh = true;
    }
    cv.notify_all();
}

void QuotaMonitor::onUpdate(std::function<void()> _callback) {
    std::lock_guard<std::mutex> lock(mutex);
    callback = std::move(_callback);
}

void QuotaMonitor::worker() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return want_refresh || stop; });

            if (stop) return;
            want_refresh = false;
        }

        Quota::Report fresh = Quota::fetch();
        std::function<void()> notify;

        {
            std::lock_guard<std::mutex> lock(mutex);
            report = std::move(fresh);
            has_report = true;
            notify = callback;
        }

        if (notify) notify();
    }
}
