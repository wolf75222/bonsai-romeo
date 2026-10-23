/* QUOTA
Explanation:
- Reads the GPFS quotas of the current user on ROMEO, with `mmlsquota -Y` (machine readable).
- Same rules as ROMEO's own report (/apps/romeotools/myquotas.sh):
    - User quotas on the home, scratch and projet filesets.
    - Group quotas on projet for every project group (named rXXXXXX) the user is in.
    - On projet the user quota has no limit: the space is counted on the project group.
    - Files left with the user's personal group inside a project don't count on the project,
      and can make writes fail: they are reported as "stray".
- Two limits per quota, for space and for inodes (every file or directory is one inode):
    - soft: can be exceeded for a grace period, after which writes are blocked
    - hard: can never be exceeded
- QuotaMonitor fetches in the background, so the UI never waits for GPFS.
*/

#pragma once

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Quota {
public:
    enum class State { OK, NO_LIMIT, NOTICE, GRACE, EXPIRED, HARD };

    struct Limit {
        uint64_t used = 0;   // Bytes for space, count for inodes
        uint64_t soft = 0;   // 0: no limit
        uint64_t hard = 0;
        std::string grace;   // "none", "5 days", "expired", ...

        bool limited() const { return soft != 0 || hard != 0; }
        State state() const;

        // Ratio of the hard limit (or soft if there is none), for gauges
        double ratio() const;
    };

    struct Row {
        bool group = false;  // Group (project) quota, or user quota
        std::string name;    // User or group name
        std::string fileset; // home, scratch, projet
        std::string path;    // Where it is mounted: /home/<user>, /scratch_p/<user>, /project/<group>

        Limit space;
        Limit inodes;

        State state() const;
    };

    struct Report {
        std::vector<Row> rows;

        // Personal group still owning data in a project (see explanation)
        std::string stray_group;
        uint64_t stray_bytes = 0;
        uint64_t stray_inodes = 0;
        std::vector<std::string> project_groups;

        std::string error;   // Set when nothing could be read
        std::chrono::system_clock::time_point fetched_at;

        State worst() const;
        int alerts() const;  // Rows needing attention (grace, expired, hard)
    };

    // Queries GPFS. Takes up to a few seconds, blocks the caller.
    static Report fetch();

    static const char* stateName(State s);
};

class QuotaMonitor {
public:
    QuotaMonitor();
    ~QuotaMonitor();

    // Latest report (empty until the first fetch is done)
    Quota::Report get();
    bool ready();

    // Asks for a new fetch, without waiting for it
    void refresh();

    // Called from the worker thread once a report is ready
    void onUpdate(std::function<void()> callback);

private:
    void worker();

    std::mutex mutex;
    std::condition_variable cv;
    Quota::Report report;
    bool has_report = false;
    bool want_refresh = true;
    bool stop = false;
    std::function<void()> callback;

    std::thread thread;
};
