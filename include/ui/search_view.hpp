/* SEARCH VIEW
- A window over the explorer (key /) to find folders by name in all of the user's spaces.
- Results follow the typing, biggest first (size, size on disk or files, like the explorer).
- Words are searched in the path, the last one in the folder name (see Scanner::search()).
- UP / DOWN select, ENTER opens the folder in the explorer, ESC closes.
- Searching ~700k folders takes a moment: it runs on its own thread (Worker), only for the latest
  query, and a search that a newer key made useless is stopped. Typing never waits for it.
- The last search is kept for the next time the window opens.
*/

#pragma once

#include "../../include/core/scanner.hpp"

#include <condition_variable>
#include <ftxui/component/component.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class BonsaiSearch {
public:
    struct State {
        std::string query;            // Edited by the input, on the UI thread
        int selected = 0;             // UI thread only

        // Written by the worker, under `mutex`
        std::mutex mutex;
        std::vector<Scanner::SearchHit> hits;
        size_t total = 0;             // Matches, even beyond the ones listed
        uint64_t version = 0;         // Bumped with every new list of hits
        bool searching = false;
        bool partial = false;         // The scan was still running
        bool open_when_ready = false; // ENTER came before the hits: the first one opens when they arrive
        int64_t now = 0;
    };

    class Worker {
    public:
        // on_result: called from the worker thread when new hits are in `state`
        Worker(std::shared_ptr<State> state, Scanner* scanner, std::function<void()> on_result);
        ~Worker();

        // Searches `query` soon; replaces any query still waiting
        void request(const std::string& query);

    private:
        void run();

        std::shared_ptr<State> state;
        Scanner* scanner;
        std::function<void()> on_result;

        std::mutex mutex;
        std::condition_variable cv;
        std::string wanted;
        uint64_t requested = 0;       // Number of the latest request
        bool stop = false;

        std::thread thread;
    };

    static ftxui::Component window(std::shared_ptr<State> state, Worker* worker, Scanner* scanner,
                                   std::function<void(const std::string& path)> on_open,
                                   std::function<void()> on_close);
};
