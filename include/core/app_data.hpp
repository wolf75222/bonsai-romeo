/* APP DATA
- Shared data between all bonsai components
*/

#pragma once

#include <ftxui/screen/color.hpp>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace ftxui;

class AppData {
public:
    /* BonsaiMenuEntry
    - Represents a single filesystem entry in the menu.
    - Contains:
      size   → folder size (0 for directories)
      label  → display name in the menu
      path   → absolute filesystem path
      is_dir → whether entry is a directory
    */
    struct BonsaiMenuEntry {
        std::uintmax_t size;
        std::uintmax_t snapped_size;
        std::string label;
        std::string path;

        bool is_dir;
        bool final = true;   // false: from the previous scan, or still being scanned (shown with ~)
        bool known = true;   // false: not in the scan, made after it or not reached yet (shown as ?)
        bool error = false;  // A folder in it couldn't be read: the size may be too low (shown with !)
        int64_t newest = 0;  // Last modification in it (seconds since 1970)
    };

    // Order of the explorer (key S); directories always come first
    enum class Sort { SIZE, NAME, AGE };

    struct BonsaiPieEntry {
        std::string label;
        Color text_color;
        Color color;

        int inner_radius;
        int outer_radius;
        double offset_angle;
        double sweep;
        int depth;
    };

    /* BonsaiMenuData
    - Shared state between UI and worker thread.
    - entries_mutex protects entries/labels updates.
    - cv_mutex + condition variable coordinate worker sleeping/waking.
    - path_changed signals directory navigation.
    - stop signals worker termination.
    */
    struct BonsaiData {
        std::mutex menu_mutex;
        std::shared_ptr<std::vector<BonsaiMenuEntry>> menu_entries;
        std::shared_ptr<std::vector<std::string>> menu_labels;

        std::mutex pie_mutex;
        std::shared_ptr<std::vector<BonsaiPieEntry>> pie_entries;

        std::shared_ptr<std::string> path;
        std::shared_ptr<int> selected;

        /* Entry to select once the menu publishes a list for `select_path` (under menu_mutex)
        - Set after F, going up or down, or opening a path from search / cleanup; "" selects the first entry
        - Resolved in that new list: the list on screen may be the old folder's, or in the old order
        */
        std::string select_path;
        std::string select_label;
        uint64_t select_generation = 0;

        // Bumped by each request above; a list built before a request (in the old order, after F) can't resolve it
        std::atomic<uint64_t> list_generation{0};

        // Bumped when what folders hold changed (a deletion): their listings are read again (see Listing)
        std::atomic<uint64_t> listing_version{0};

        std::atomic<Sort> sort{Sort::SIZE};

        // UI thread only: the age column (key A), and the entries marked with SPACE (real paths)
        bool show_age = false;
        std::set<std::string> marked;

        std::mutex cv_mutex;
        std::condition_variable cv;

        bool menu_path_changed = false;
        bool pie_path_changed = false;
        bool pie_sel_changed = false;
        bool stop = false;

    };

    /* stop()
    - Sets done flag to true.
    - Any thread created will a data struct is watching that flag.
    - Intended to be called during application exit.
    */
    static void stop(std::shared_ptr<AppData::BonsaiData> data);
};
