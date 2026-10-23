/* PROGRESS
- What a long job (deleting, packing a folder, scanning one again) has done so far, for the progress window.
- Written by the job's threads, read by the UI while it runs: counters are atomics, and each thread keeps
  the entry it is on in a slot of its own, with its own lock, so a thread never waits for the UI or another thread.
- `stop` asks the job to stop: what is done stays done, the rest is left as it was.
*/

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class Progress {
public:
    std::atomic<uint64_t> done{0};     // Entries done (deleted, packed, scanned)
    std::atomic<uint64_t> total{0};    // Expected (0: unknown), from the scan
    std::atomic<uint64_t> disk{0};     // On disk, all of it: what a deletion frees, little by little
    std::atomic<uint64_t> checked{0};  // Folders checked before deleting anything
    std::atomic<bool> stop{false};

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    static constexpr size_t SLOTS = 32;

    // What the job does now ("Deleting", "Checking the archive"...), and a new count for it
    void step(const std::string& what, uint64_t expected = 0) {
        {
            std::lock_guard<std::mutex> lock(step_mutex);
            current = what;
            step_started = std::chrono::steady_clock::now();
        }
        done = 0;
        total = expected;
    }

    std::string stepName() {
        std::lock_guard<std::mutex> lock(step_mutex);
        return current;
    }

    // Seconds since the current step started
    double stepSeconds() {
        std::lock_guard<std::mutex> lock(step_mutex);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - step_started).count();
    }

    // The entry thread `slot` is on (every few entries: it takes a lock)
    void note(size_t slot, const std::string& entry) {
        Slot& s = slots[slot % SLOTS];
        const uint64_t at = clock.fetch_add(1) + 1;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.entry = entry;
        s.at = at;
    }

    // The entries noted last, newest first
    std::vector<std::string> recent(size_t count) {
        std::vector<std::pair<uint64_t, std::string>> all;
        for (auto& s : slots) {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.entry.empty())
                all.emplace_back(s.at, s.entry);
        }

        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

        std::vector<std::string> out;
        for (size_t i = 0; i < all.size() && i < count; i++)
            out.push_back(std::move(all[i].second));
        return out;
    }

private:
    struct Slot {
        std::mutex mutex;
        std::string entry;
        uint64_t at = 0;
    };

    std::mutex step_mutex;
    std::string current;
    std::chrono::steady_clock::time_point step_started = std::chrono::steady_clock::now();

    std::array<Slot, SLOTS> slots;
    std::atomic<uint64_t> clock{0};
};
