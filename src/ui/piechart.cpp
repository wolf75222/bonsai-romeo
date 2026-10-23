#include "../../include/ui/piechart.hpp"

#include "../../include/config/config.hpp"
#include "../../include/utils/format.hpp"

#include <filesystem>
#include <algorithm>
#include <thread>
#include <string>
#include <cmath>
#include <map>
#include <set>
#include <sys/ioctl.h>
#include <unistd.h>

void BonsaiPie::drawAngledBlockEllipseRingOffset(Canvas& c, int cx, int cy, int r1, int r2, int r_inner, double start_deg, double sweep_deg, const std::string& label, const Color& color, const Color& text_color, std::vector<int>& drawn_by, int slice_id, std::vector<Label>& labels) {
    if (sweep_deg <= 0.0)
        return;

    const double start = start_deg * M_PI / 180.0;
    const double end   = (start_deg + sweep_deg) * M_PI / 180.0;

    /* Angle step — controls smoothness vs speed
    - Smaller = smoother but slower
    - 0.3 deg is usually visually perfect in terminal
    - Make it user adjustable? idk.
    */
    const double step = 0.3 * M_PI / 180.0;

    double cos_a = std::cos(start);
    double sin_a = std::sin(start);

    // Precompute rotation delta (incremental rotation)
    const double cos_d = std::cos(step);
    const double sin_d = std::sin(step);

    // One radial sample per dot of the longer axis, so no block is skipped
    const int outer_r = std::max(r1, r2);
    const double inner_t = r1 > 0 ? static_cast<double>(r_inner) / r1 : 0.0;
    const int first_r = static_cast<int>(inner_t * outer_r);

    // Label in the middle of the slice. A character is 2 dots wide on the canvas
    const double mid_angle = start + (end - start) / 2.0;
    const double mid_t = (inner_t + 1.0) / 2.0;

    /* Label length: what fits in the slice, on the row of the label
    - Measured: from the middle of the slice, dot by dot to the left and to the right, while still
      inside the slice (between its two radii and its two angles)
    - Up to 16 characters ("pops_adc_comp.."): upstream cut every name to 3 ("pop..")
    - No label at all when neither the name nor 3 of its letters fit, or the slice is less than a
      row high there: it would only cover its neighbours
    */
    const double y_ratio = r1 > 0 ? static_cast<double>(r2) / r1 : 1.0;
    const double center_x = cx + std::cos(mid_angle) * r1 * mid_t;
    const double center_y = cy + std::sin(mid_angle) * r2 * mid_t;

    auto inside = [&](double px, double py) {
        const double dx = px - cx;
        const double dy = (py - cy) / y_ratio;   // Back to a round pie
        const double r = std::hypot(dx, dy);
        if (r < r_inner || r > r1)
            return false;

        double a = std::atan2(dy, dx);
        while (a < start) a += 2 * M_PI;
        while (a >= start + 2 * M_PI) a -= 2 * M_PI;
        return a <= end;
    };

    int left = 0;
    while (left < 40 && inside(center_x - left - 1, center_y)) left++;
    int right = 0;
    while (right < 40 && inside(center_x + right + 1, center_y)) right++;

    // A label of n characters is 2n dots wide, n on each side of the middle; one dot of margin
    const int room = std::min(left, right) - 1;
    const bool tall_enough = inside(center_x, center_y - 1.5) && inside(center_x, center_y + 1.5);

    // The whole name, or at least 3 of its letters: "h.." says nothing
    const size_t length = FormatUtils::width(label);
    std::string display_label;
    if (tall_enough && room >= 3 && (length <= static_cast<size_t>(room) || room >= 5)) {
        display_label = FormatUtils::trimSuffix(label, static_cast<size_t>(std::min(room, 16)), "..");
    }

    const int text_x = static_cast<int>(center_x) - static_cast<int>(FormatUtils::width(display_label));
    const int text_y = static_cast<int>(center_y);

    const int width = c.width();
    const int height = c.height();

    // Built once per slice instead of once per block
    const Canvas::Stylizer block_style = [color](Cell &c) {
        c.foreground_color = color;
    };

    for (double a = start; a < end; a += step) {
        const double dx_outer = cos_a * r1;
        const double dy_outer = sin_a * r2;

        const double inv_len = 1.0 / outer_r;

        for (int r = first_r; r <= outer_r; ++r) {
            double t = r * inv_len;

            int px = static_cast<int>(cx + dx_outer * t);
            int py = static_cast<int>(cy + dy_outer * t);

            // Outside of the canvas, nothing would be drawn
            if (px < 0 || px >= width || py < 0 || py >= height)
                continue;

            /* Performance:
            - The angle steps are much finer than the blocks, so most steps land on a block this slice already drew
            - Drawing a block twice with the same color changes nothing, and DrawBlock is the expensive part
            - So each block is only drawn the first time this slice reaches it
            */
            int& owner = drawn_by[static_cast<size_t>(py / 2) * width + px];
            if (owner == slice_id)
                continue;

            owner = slice_id;
            c.DrawBlock(px, py, true, block_style);
        }

        double new_cos = cos_a * cos_d - sin_a * sin_d;
        double new_sin = sin_a * cos_d + cos_a * sin_d;

        cos_a = new_cos;
        sin_a = new_sin;
    }

    if (!display_label.empty())
        labels.push_back(Label{text_x, text_y, display_label, color, text_color});
}

double BonsaiPie::cellRatio() {
    const double configured = Config::get().CHART_CELL_RATIO;
    if (configured > 0)
        return configured;

    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_xpixel > 0 && ws.ws_ypixel > 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        const double ratio = (static_cast<double>(ws.ws_ypixel) / ws.ws_row) / (static_cast<double>(ws.ws_xpixel) / ws.ws_col);

        // Anything else is a terminal reporting nonsense
        if (ratio > 1.0 && ratio < 4.0)
            return ratio;
    }

    return 2.0;
}

// Same as Color::Interpolate(t, color, Color::Black) (gamma 2.2), on exact RGB values
static std::array<int, 3> darken(const std::array<int, 3>& rgb, double t) {
    t = std::clamp(t, 0.0, 1.0);
    std::array<int, 3> out;

    for (int i = 0; i < 3; i++)
        out[i] = static_cast<int>(std::pow(std::pow(rgb[i], 2.2) * (1.0 - t), 1.0 / 2.2));

    return out;
}

// Towards white by `t` (0..1): the folders inside the selection
static std::array<int, 3> lighten(const std::array<int, 3>& rgb, double t) {
    std::array<int, 3> out;
    for (int i = 0; i < 3; i++)
        out[i] = static_cast<int>(rgb[i] + (255 - rgb[i]) * t);
    return out;
}

// An entry gets its own slice when it weighs at least `threshold` percent of the displayed directory
static bool reachesThreshold(uint64_t size, uint64_t root_size, double threshold) {
    return root_size > 0 && size <= root_size && size * 100.0 / root_size >= threshold;
}

void BonsaiPie::collectEntries(const fs::path& dir, std::vector<EntryInfo>& entries, int current_depth, int max_depth, uint64_t root_size, double threshold, Scanner* scanner, Listing& listing, uint64_t version) {
    if (current_depth > max_depth) return;

    // Above the spaces: the spaces themselves, like the menu (see Scanner::isSpacesView)
    if (scanner->isSpacesView(dir)) {
        for (const auto& root : scanner->getScope().roots) {
            const uint64_t size = scanner->get(root);
            entries.push_back(EntryInfo{root, size, true, current_depth});

            if (reachesThreshold(size, root_size, threshold))
                collectEntries(root, entries, current_depth + 1, max_depth, root_size, threshold, scanner, listing, version);
        }
        return;
    }

    // What the folder holds, from the listing (read once, see Listing); folder sizes from the scan
    const Listing::Entries listed = listing.get(dir, version);
    for (const auto& entry : *listed) {
        // Same filter as the menu, so the selection matches the slices
        if (!scanner->visibleEntry(entry.path, entry.is_dir, entry.owned))
            continue;

        uint64_t size = 0;

        if (entry.is_dir) {
            size = scanner->get(entry.path);
        } else if (entry.regular) {
            size = scanner->fileValue(entry.bytes, entry.disk);
        } else {
            continue;
        }

        /* Performance:
        - Below the top level, an entry under the size threshold is never drawn
        - Neither is anything inside of it, as a child can't be larger than its parent
        - So these entries are dropped right here and their sub-directories are never opened,
          which is most of the tree: at most 100 / threshold entries per ring can reach the threshold
        - Top-level entries are all kept: the selection and CHART_MIN_SLICES need them
        */
        const bool has_slice = reachesThreshold(size, root_size, threshold);

        if (current_depth > 0 && !has_slice)
            continue;

        entries.push_back(EntryInfo{entry.path, size, entry.is_dir, current_depth});

        if (entry.is_dir && has_slice)
            collectEntries(entry.path, entries, current_depth + 1, max_depth, root_size, threshold, scanner, listing, version);
    }
}

void BonsaiPie::worker(ScreenInteractive* screen, std::shared_ptr<AppData::BonsaiData> data, Scanner* scanner, const fs::path& default_path) {
    std::vector<EntryInfo> entries;

    // What the folders drawn hold, read once and kept (see Listing)
    Listing listing(scanner->getScope().uid);

    // True when `entries` was listed during the scan, with sizes that were still growing
    bool entries_from_scan = false;

    // Directory `entries` was listed for
    fs::path entries_path;

    int passes = 0;

    while (true) {

        fs::path current_path = "";
        bool sel_changed = false;
        int selected = 0;

        // The wake-up flags are written under cv_mutex (and waited on with it, below)
        {
            std::lock_guard<std::mutex> lock(data->cv_mutex);
            sel_changed = data->pie_sel_changed;
            data->pie_sel_changed = false;
        }

        {
            std::lock_guard<std::mutex> lock(data->menu_mutex);
            current_path = *data->path;

            // If the top label is ".." it means we have to offset selected index by -1
            if (*data->selected >= 0 && *data->selected < (int)data->menu_entries->size()) {
                selected = current_path != default_path ? *data->selected - 1 : *data->selected;
            }
        }

        // Read before any size: if the scan is done at this point, every size read below is final
        bool scan_done = scanner->isDone();

        // Get size of current dir
        uint64_t root_size = scanner->get(current_path);
        Config cfg = Config::get();

        /* Only recompute children if path has changed
        - Or if they were listed during the scan: entries are filtered by size while listing,
          so a list made with unfinished sizes can't be kept
        - The path is compared too: when a selection change and a new path arrive together
          (arrow key then enter, handled in the same frame), sel_changed is set although the path changed.
          The pie then showed the entries of the previous directory until the next key press
        */
        if(!sel_changed || entries_from_scan || current_path != entries_path){
            // A folder just opened is read again, as in the explorer
            if (current_path != entries_path)
                listing.expire(current_path, Listing::RECENT);

            entries_from_scan = !scan_done;
            entries_path = current_path;
            entries.clear();

            // Parse current path with a max depth of 3
            collectEntries(current_path, entries, 0, cfg.CHART_MAX_GENERATIONS - 1, root_size, cfg.CHART_MAX_SIZE_THRESHOLD_PERCENTAGE, scanner, listing, data->listing_version.load());

            /* Sort:
            - Depth first: lower depth is higer priority
            - Type second: directories have a higher priority over files
            - Size third: larger sizes have a higher priority over lower ones
            - Name last: equally sized entries are in the same order as in the menu

            - Maybe we could use a priority queue here? idk.
            */
            std::sort(entries.begin(), entries.end(), [](const EntryInfo& a, const EntryInfo& b) {
                if (a.depth != b.depth)
                    return a.depth < b.depth;
                if (a.is_dir != b.is_dir)
                    return a.is_dir > b.is_dir;
                if (a.size != b.size)
                    return a.size > b.size;
                return a.path.native() < b.path.native();
            });
        }

        std::vector<AppData::BonsaiPieEntry> slices;

        int i = 0;

        /* Color & offset tracking:
        - layer_offsets_per_parent:
          Tracks the accumulated sweep (in degrees) already used for a given parent
          at its current depth layer. This determines where the next child slice
          of that parent should start (running angular total per parent).

        - slice_offsets:
          Stores the absolute starting angle of each slice (keyed by full path).
          Used so that children can inherit their parent’s base offset and stack
          correctly inside the parent’s angular span.

        - slice_colors:
          Stores the computed color and text color for each slice (keyed by full path).
          Allows child entries to derive their color from their parent’s color
          (e.g., darkening via interpolation).

        - color_indexes:
          Tracks how many slices have already been processed per depth layer per parent.
          Used to progressively adjust (e.g., darken) sibling slice colors within
          the same depth.
        */
        std::map<std::string, double> layer_offsets_per_parent;
        std::map<std::string, double> slice_offsets;

        std::map<std::pair<int, std::string>, int> color_indexes_per_parent;

        /* Slice colors are kept as RGB values
        - A child is its parent's color, darkened. With 256 colors, darkening the already rounded
          palette color and rounding again often fell back on the parent's color: rings looked flat.
        - Darkening the exact RGB values and rounding once keeps the shades apart.
        */
        struct SliceColor {
            std::array<int, 3> rgb;
            Color text;
            bool selected;   // In the selected folder (shown paler)
        };
        std::map<std::string, SliceColor> slice_colors;

        /* Minimum slices:
        - If no top-level entry reaches the size threshold (e.g. lots of equally sized entries),
          every slice gets filtered out and the pie stays empty
        - In that case the CHART_MIN_SLICES largest top-level entries are drawn anyway, at their real size
        - Nothing is forced as soon as one top-level entry reaches the threshold
        */
        std::set<const EntryInfo*> forced_slices;

        if(root_size > 0 && cfg.CHART_MIN_SLICES > 0) {
            std::vector<const EntryInfo*> candidates;
            bool has_visible_slice = false;

            for(const auto& entry : entries) {
                // Entries are sorted by depth, so top-level ones come first
                if(entry.depth != 0) {
                    break;
                }

                if(entry.size == 0 || entry.size > root_size) {
                    continue;
                }

                if(entry.size * 100.0 / root_size >= cfg.CHART_MAX_SIZE_THRESHOLD_PERCENTAGE) {
                    has_visible_slice = true;
                    break;
                }

                candidates.push_back(&entry);
            }

            if(!has_visible_slice) {
                // Stable: equally sized entries keep their current order
                std::stable_sort(candidates.begin(), candidates.end(), [](const EntryInfo* a, const EntryInfo* b) {
                    return a->size > b->size;
                });

                size_t count = std::min(candidates.size(), static_cast<size_t>(cfg.CHART_MIN_SLICES));
                forced_slices.insert(candidates.begin(), candidates.begin() + count);
            }
        }

        for(auto& entry : entries) {
            if(root_size <= 0) {
                continue;
            }

            if(entry.size > root_size) {
                continue;
            }

            /* Performance:
            - I choose only to keep 2 rings active during scanning
            - Many rings imply an even larger amount of slices
            - This causes a lot of DrawAngledBlockEllipseOffset(...) calls which are performance intensive on ui thread
            - Ftxui has cell buffer when rendering so 1rst render takes a long time with 4 rings (lots of cell updates)
            - 4 ring piechart is completed only on scan completion
            */
            if(entry.depth >= 2 && !scanner->isDone()) {
                continue;
            }

            /* Pie geometry:
            - Maybe make some of this user-customizable in the future.
            */
            int inner_hole_radius = 10;
            int inner_radius = inner_hole_radius + ((entry.depth + 1) * 25);
            int outer_radius = inner_radius + 25;

            // Kept as doubles: integer math dropped several degrees per slice, which left gaps in the rings
            double occupancy = entry.size * 100.0 / root_size;
            double sweep = entry.size * 360.0 / root_size;

            if(occupancy < cfg.CHART_MAX_SIZE_THRESHOLD_PERCENTAGE && !forced_slices.count(&entry)) {
                continue;
            }

            std::array<int, 3> rgb = cfg.CHART_COLORS[i % cfg.CHART_COLORS.size()];
            Color text_color = Color::Default;

            /* Shades of the children
            - Each child is darker than the previous one: 1, 2, 3, 4 steps of CHART_DIM_FACTOR, then again from 1
            - Counting on (5, 6, 7... steps) ended in black slices for folders with many sub-folders
            */
            if(entry.depth != 0) {
                std::string parent = entry.path.parent_path().string();

                auto key = std::make_pair(entry.depth, parent);
                const int index = color_indexes_per_parent[key]++;

                rgb = darken(slice_colors[parent].rgb, cfg.CHART_DIM_FACTOR * (index % 4 + 1));
                text_color = slice_colors[parent].text;
            }

            const bool in_selection = entry.depth != 0 && slice_colors[entry.path.parent_path().string()].selected;

            // Inside the selected folder: paler, still in its colors (upstream turned it all gray)
            const std::array<int, 3> shown = in_selection && !cfg.CHART_HIGHLIGHT_SUBTREE ? lighten(rgb, 0.45) : rgb;
            Color display_color = Color::RGB(shown[0], shown[1], shown[2]);
            Color display_text = in_selection && !cfg.CHART_HIGHLIGHT_SUBTREE ? Color::Black : text_color;
            bool selected_here = in_selection;

            /* In these two code blocks, false is the logic to check wether the slice is selected
            - selected can be -1 because we manually add a back entry for non default_dir paths
            - Only the selected slice turns white, its sub-folders keep their colors: when the selection
              was a big folder (82% of the disk), the whole pie went white and gray
            - CHART_HIGHLIGHT_SUBTREE = true brings back upstream's look (whole subtree in grays)
            */
            if(selected != -1 && entry.depth == 0 && selected < entries.size() && entries[selected].depth == 0) {
                // Same element of the list: no need to ask the filesystem if the two paths are the same file
                bool is_selected = &entry == &entries[selected];

                if (is_selected) {
                    display_color = Color::RGB(255, 255, 255);
                    display_text = Color::Black;
                    selected_here = true;

                    if (cfg.CHART_HIGHLIGHT_SUBTREE) {
                        rgb = {255, 255, 255};
                        text_color = Color::Black;
                    }
                }
            }

            slice_colors[entry.path.string()] = SliceColor{rgb, text_color, selected_here};
            Color color = display_color;
            text_color = display_text;

            AppData::BonsaiPieEntry slice;
            slice.depth = entry.depth;
            slice.color = color;
            slice.text_color = text_color;
            slice.inner_radius = inner_radius;
            slice.outer_radius = outer_radius;
            // A space is named after what it is: "home", "scratch", "r250127" (its folder is just the user's name)
            const std::string space = scanner->getScope().shortName(entry.path.string());
            slice.label = space.empty() ? entry.path.filename().string() : space;
            slice.sweep = sweep;

            if(entry.depth == 0) {
                slice.offset_angle = layer_offsets_per_parent[current_path];
                layer_offsets_per_parent[current_path] += sweep;

            } else {
                std::string parent = entry.path.parent_path().string();
                slice.offset_angle = slice_offsets[parent] + layer_offsets_per_parent[parent];
                layer_offsets_per_parent[parent] += sweep;
            }

            slice_offsets[entry.path.string()] = slice.offset_angle;
            slices.push_back(slice);

            i++;
        }

        /* Possible optimisation
        - It should be possible to concatenate slices on the same layer and have a different method than DrawAngledBlockEllipseOffset(...) be called for each slice of each layer
        - This should save on rendering time as we only have to calculate our pie for each layer instead of for each slice of each layer.
        - We'll see.
        */

        {
            // Publish results
            std::lock_guard<std::mutex> lock(data->pie_mutex);
            *data->pie_entries = std::move(slices);
        }

        // Update render
        screen->PostEvent(Event::Custom);

        /* Bug fix:
        - On first render the pie will be updated with new size but not new folders
        - Probably caused by a desync between menu and pie threads? idk.
        - This forces a single extra pass/iteration (recompute everything on the first scan complete)
        */
        if(passes == 0 && scanner->isDone()) {
            passes++;
            continue;
        }

        // Sleep until woken up or keep going if scanner hasn't completed
        if(!scanner->isDone()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }


        {
            /* Same mutex as the writers of the flags: with another one (it used pie_mutex), a wake-up
               could be missed, the stop signal too, and quitting then hung on joining this thread */
            std::unique_lock<std::mutex> lock(data->cv_mutex);

            data->cv.wait(lock, [&] {
                return data->pie_path_changed || data->pie_sel_changed || data->stop;
            });

            if (data->stop)
                break;

            if (data->pie_path_changed)
                data->pie_path_changed = false;
        }
    }
}

Component BonsaiPie::pie(std::shared_ptr<AppData::BonsaiData> data, Scanner* scanner, const fs::path& default_path) {
    return Renderer([data, scanner] {
        return canvas([data, scanner](Canvas& c) {
            int w = c.width();
            int h = c.height();

            std::vector<AppData::BonsaiPieEntry> entries;
            fs::path current_path;

            {
                std::lock_guard<std::mutex> lock(data->pie_mutex);
                entries = *data->pie_entries;
                current_path = *data->path;
            }

            uint64_t current_size = scanner->get(current_path);
            std::string label = scanner->format(current_size, " ");
            if (scanner->getMetric() == Scanner::Metric::INODES)
                label += " files";

            /* Round pie on screen
            - The canvas has 2 dots per cell across and 4 down, so a dot is ratio / 2 times taller than wide
            - A circle `r` dots across is therefore `r * 2 / ratio` dots down
            - Radii below are along X; Y radii are scaled by y_scale
            */
            const double y_scale = 2.0 / cellRatio();

            // Account for margin (15 dots) and center hole (15%)
            int max_radius = std::min(w / 2 - 15, static_cast<int>((h / 2 - 15) / y_scale));
            // The hole holds the total: a character is 2 dots wide, so half the label is `length` dots
            int inner_hole_radius = std::min(std::max(static_cast<int>(max_radius * 0.15), static_cast<int>(FormatUtils::width(label)) + 2), max_radius / 2);

            Config cfg = Config::get();
            int max_layers = cfg.CHART_MAX_GENERATIONS;

            // The rings drawn share the room: 2 rings during the scan, up to CHART_MAX_GENERATIONS after it
            int rings = 1;
            for (const auto& entry : entries)
                rings = std::max(rings, entry.depth + 1);
            rings = std::min(rings, std::max(max_layers, 1));

            int available_radius = max_radius - inner_hole_radius;
            int layer_thickness = available_radius / rings;

            // One marker per block of the canvas: the last slice that drew it (see drawAngledBlockEllipseRingOffset)
            std::vector<int> drawn_by(static_cast<size_t>(std::max(w, 0)) * (std::max(h, 0) / 2 + 1), -1);
            int slice_id = 0;
            std::vector<BonsaiPie::Label> labels;

            for (auto& entry : entries) {

                int inner_radius = inner_hole_radius + entry.depth * layer_thickness;
                int outer_radius = inner_radius + layer_thickness;

                BonsaiPie::drawAngledBlockEllipseRingOffset(
                    c,
                    w / 2,
                    h / 2,
                    outer_radius,
                    static_cast<int>(outer_radius * y_scale + 0.5),
                    inner_radius,
                    entry.offset_angle,
                    entry.sweep,
                    entry.label,
                    entry.color,
                    entry.text_color,
                    drawn_by,
                    slice_id++,
                    labels
                );
            }

            for (const auto& label : labels) {
                c.DrawText(label.x, label.y, label.text, [&label](Cell &cell) {
                    cell.foreground_color = label.text_color;
                    cell.background_color = label.color;
                });
            }

            // A character is 2 dots wide: half the label is `length` dots
            c.DrawText(w / 2 - static_cast<int>(FormatUtils::width(label)), h / 2, label);

            // Scanner status
            if(!scanner->isDone()) {
                const std::string status = "Scanning...";
                c.DrawText(w - 2 * static_cast<int>(status.size()) - 2, 0, status, [](Cell& c) { c.foreground_color = Color::White; });
            }

        }) | flex;
    });
}
