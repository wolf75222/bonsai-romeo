/* CONFIG
Explanation:
- This class handles loading Bonsai's configuration from ~/.config/bonsai-romeo/bonsai.conf
- It provides default values if the file doesn't exist
- Configuration values include sidebar icons, chart parameters, colors, and UI sizing
- Singleton pattern ensures only one Config object exists at runtime
- The parser reads key=value lines, trims whitespace, and converts to the appropriate type
- Complex values (like CHART_COLORS) are parsed from array blocks in the config
- All file I/O, directory creation, and parsing errors are safely handled
*/

#pragma once

#include "gen/defaults.hpp"

#include <ftxui/component/component.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

class Config {
public:
    std::vector<std::array<int, 3>> CHART_COLORS;

    std::string SIDEBAR_SELECTED_FOLDER_ICON;
    std::string SIDEBAR_SELECTED_FILE_ICON;
    std::string SIDEBAR_FOLDER_ICON;
    std::string SIDEBAR_FILE_ICON;
    std::string SIDEBAR_BACK_ICON;
    std::string COLOR_MODE = "auto";

    std::array<int, 3> SIDEBAR_GROWTH_COLOR = {255, 175, 95};
    std::array<int, 3> SIDEBAR_SHRINK_COLOR = {95, 125, 255};

    int CHART_MAX_GENERATIONS = 4;
    int CHART_MIN_SLICES = 10;
    int SIDEBAR_WIDTH = 40;
    int SCANNER_THREADS = 0;
    int CLEANUP_OLD_DAYS = 180;

    double SIDEBAR_GROWTH_THRESHOLD_PERCENTAGE = 0.2;
    double CHART_MAX_SIZE_THRESHOLD_PERCENTAGE = 2;
    double CHART_DIM_FACTOR = 0.15;
    double CHART_CELL_RATIO = 0;

    bool ENABLE_FOLDER_SIZE_PERCENTAGES = true;
    bool CHART_HIGHLIGHT_SUBTREE = false;

    /* Lazy initialization using a lambda:
    - Ensures config is loaded once at first access
    - If the config file does not exist, create it with defaults
    - Parse the file over the embedded defaults; if it can't be read or parsed, the defaults alone
    */
    static const Config& get() {
        static Config instance = []() -> Config {
            /* Never fatal
            - The file can't be created when the home is over its quota (or read-only): that's precisely
              when bonsai-romeo is needed, so the embedded defaults are used instead of throwing
            */
            // The embedded defaults first: a key missing from the user's file (an older one) keeps its default
            std::istringstream defaults(std::string(reinterpret_cast<const char*>(bonsai_default), bonsai_default_len));
            const Config base = parseConfig(defaults).value_or(Config());

            std::optional<Config> config;
            if (ensureConfigFile()) {
                std::ifstream in(getUserConfigPath());
                if (in.is_open())
                    config = parseConfig(in, base);
            }

            Config cfg = config ? *config : base;

            // The pie picks colors from this list: never empty (it divides by its size)
            if (cfg.CHART_COLORS.empty())
                cfg.CHART_COLORS = base.CHART_COLORS;
            if (cfg.CHART_COLORS.empty())
                cfg.CHART_COLORS = {{255, 99, 71}, {30, 144, 255}, {60, 179, 113}, {255, 165, 0}};

            return cfg;
        }();
        return instance;
    }

    /* Save the config file:
    - If user resizes the sidebar, then it's saved here
    - That is the only thing that can change programmatically, for now
    - All other keys are copied verbatim from the user's existing file so
      manual edits (comments, formatting, etc.) survive the rewrite
    - Nothing is written when nothing changed, and the new file replaces the old one only once it is fully
      written: a home over its quota used to leave an empty config behind
    */
    bool writeToFile() const {
        std::string path = getUserConfigPath();
        if (path.empty())
            return false;

        std::ifstream in(path);
        if (!in.is_open())
            return false;

        std::string userConfig(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>()
        );
        in.close();

        std::string config(
            reinterpret_cast<const char*>(bonsai_default),
            bonsai_default_len
        );

        // Every field's key lives in fieldTable() exactly once; passthrough
        // fields get copied verbatim from the user's existing file.
        for (const auto& field : fieldTable()) {
            if (field.passthrough && !isLegacyIcon(userConfig, field.key)) {
                copyValue(config, userConfig, field.key);
            }
        }

        // CHART_COLORS spans several lines: the user's block is copied whole
        auto [user_start, user_len] = findBlockSpan(userConfig, "CHART_COLORS");
        auto [config_start, config_len] = findBlockSpan(config, "CHART_COLORS");
        if (user_start != std::string::npos && config_start != std::string::npos)
            config.replace(config_start, config_len, userConfig.substr(user_start, user_len));

        // SIDEBAR_WIDTH is the one field this class can change at runtime,
        // so we write our in-memory value instead of copying from disk.
        copyValue(config, "SIDEBAR_WIDTH = " + std::to_string(SIDEBAR_WIDTH), "SIDEBAR_WIDTH");

        if (config == userConfig)
            return true;

        // Written next to the file (the one a symlink points to), then renamed over it
        std::error_code ec;
        fs::path target = fs::weakly_canonical(path, ec);
        if (ec) target = path;
        const fs::path tmp = target.string() + ".tmp";

        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;

        out.write(config.data(), static_cast<std::streamsize>(config.size()));
        out.close();

        if (out.good())
            fs::rename(tmp, target, ec);
        if (!out.good() || ec) {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    }

    static ftxui::Color growthColor() {
        const auto& config = Config::get();
        return ftxui::Color::RGB(
            config.SIDEBAR_GROWTH_COLOR[0],
            config.SIDEBAR_GROWTH_COLOR[1],
            config.SIDEBAR_GROWTH_COLOR[2]
        );
    }

    static ftxui::Color shrinkColor() {
        const auto& config = Config::get();
        return ftxui::Color::RGB(
            config.SIDEBAR_SHRINK_COLOR[0],
            config.SIDEBAR_SHRINK_COLOR[1],
            config.SIDEBAR_SHRINK_COLOR[2]
        );
    }

private:
    // Returns path to ~/.config/bonsai-romeo/bonsai.conf or empty string if HOME not set
    static std::string getUserConfigPath() {
        // BONSAI_CONFIG: another config file (tests, or several setups)
        if (const char* env = std::getenv("BONSAI_CONFIG"); env && *env)
            return env;

        if (const auto* pw = getpwuid(getuid()); pw && pw->pw_dir) {
            return (fs::path(pw->pw_dir) / ".config/bonsai-romeo/bonsai.conf").string();
        }
        return ".config/bonsai-romeo/bonsai.conf";
    }

    // Writes the default configuration binary to the given path
    static bool writeDefaultConfig(const std::string& path) {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;

        out.write(reinterpret_cast<const char*>(bonsai_default), bonsai_default_len);
        out.close();

        // A half-written file (home over quota) would be read back as an incomplete config
        if (!out.good()) {
            std::error_code ec;
            fs::remove(path, ec);
            return false;
        }
        return true;
    }

    /* Ensures the config file exists:
    - Creates parent directories if needed
    - Writes default config if file doesn't exist
    - Returns true if file is ready
    */
    static bool ensureConfigFile() {
        std::string config_path = getUserConfigPath();
        fs::path config_dir = fs::path(config_path).parent_path();

        std::error_code ec;
        if (!fs::exists(config_dir, ec)) fs::create_directories(config_dir, ec);
        if (!fs::exists(config_path, ec)) return writeDefaultConfig(config_path);

        return true;
    }

    /* Legacy icons
    - Upstream's defaults are Nerd Font icons: their width depends on the terminal and the font,
      which shifts the sidebar columns. A config still holding them gets the new ASCII defaults.
    - Icons the user picked themselves are kept.
    */
    static bool isLegacyIconValue(const std::string& value) {
        static const char* legacy[] = {"\"\uf07b\"", "\"\uf15b\"", "\"\uf114\"", "\"\uf4a5\"", "\"\uf0e2\""};
        for (const char* l : legacy)
            if (value == l) return true;
        return false;
    }

    static bool isLegacyIcon(const std::string& source, const std::string& key) {
        auto [start, len] = findValueSpan(source, key);
        return start != std::string::npos && isLegacyIconValue(source.substr(start, len));
    }

    static std::string defaultValue(const std::string& key) {
        const std::string defaults(reinterpret_cast<const char*>(bonsai_default), bonsai_default_len);
        auto [start, len] = findValueSpan(defaults, key);
        return start == std::string::npos ? "" : defaults.substr(start, len);
    }

    static void trim(std::string& s) {
        size_t start = s.find_first_not_of(" \t\r");
        if (start == std::string::npos) { s.clear(); return; }
        size_t end = s.find_last_not_of(" \t\r");
        s = s.substr(start, end - start + 1);
    }

    /* Where a comment starts in a value, or npos
    - A '#' inside quotes is part of the value (an icon can be "#")
    */
    static size_t commentStart(const std::string& s, size_t from = 0) {
        bool quoted = false;
        for (size_t i = from; i < s.size() && s[i] != '\n'; i++) {
            if (s[i] == '"') quoted = !quoted;
            else if (s[i] == '#' && !quoted) return i;
        }
        return std::string::npos;
    }

    /* Finds the '=' after a key
    - The key must start its line: not in a comment, nor at the end of a longer key
    */
    static size_t findKey(const std::string& source, const std::string& key) {
        for (size_t pos = source.find(key); pos != std::string::npos; pos = source.find(key, pos + 1)) {
            size_t line = pos;
            while (line > 0 && (source[line - 1] == ' ' || source[line - 1] == '\t')) --line;
            if (line > 0 && source[line - 1] != '\n') continue;

            size_t eq = pos + key.size();
            while (eq < source.size() && (source[eq] == ' ' || source[eq] == '\t')) ++eq;
            if (eq < source.size() && source[eq] == '=') return eq;
        }
        return std::string::npos;
    }

    /* Finds value in config file for a given key
    - returns the starting index of the value string, and its length
    - for example, findValueSpan("PORT=8080", "PORT") would return <5, 4>
    */
    static std::pair<size_t, size_t> findValueSpan(const std::string& source, const std::string& key) {
        size_t eq = findKey(source, key);
        if (eq == std::string::npos) return {std::string::npos, 0};

        size_t value_start = eq + 1;
        while (value_start < source.size() &&
               (source[value_start] == ' ' || source[value_start] == '\t')) {
            ++value_start;
        }

        size_t value_end = value_start;
        const size_t comment = commentStart(source, value_start);
        while (value_end < source.size() &&
               value_end != comment &&
               source[value_end] != '\n' &&
               source[value_end] != '\r') {
            ++value_end;
        }

        while (value_end > value_start &&
               (source[value_end - 1] == ' ' || source[value_end - 1] == '\t')) {
            --value_end;
        }

        return {value_start, value_end - value_start};
    }

    // Span of a value written as [ ... ] over several lines, brackets included
    static std::pair<size_t, size_t> findBlockSpan(const std::string& source, const std::string& key) {
        auto [start, len] = findValueSpan(source, key);
        if (start == std::string::npos || len == 0 || source[start] != '[') return {std::string::npos, 0};

        const size_t end = source.find(']', start);
        if (end == std::string::npos) return {std::string::npos, 0};
        return {start, end + 1 - start};
    }

    /* Replaces the value for a given key in the config string with the value for that key from the source string
    - for example, copyValue("PORT=8080", "PORT=3000", "PORT") would change "PORT=8080" to "PORT=3000"
    */
    static bool copyValue(std::string& config, const std::string& source, const std::string& key) {
        auto [source_start, source_len] = findValueSpan(source, key);
        if (source_start == std::string::npos) return false;
        std::string value = source.substr(source_start, source_len);

        auto [config_start, config_len] = findValueSpan(config, key);
        if (config_start == std::string::npos) return false;

        config.replace(config_start, config_len, value);
        return true;
    }

    static std::string unquote(const std::string& value) {
        if (value.size() < 2) return value;
        return value.substr(1, value.size() - 2);
    }

    static std::array<int, 3> parseColor(const std::string& value) {
        std::stringstream ss(value);
        std::array<int, 3> color{};
        char c;
        ss >> c >> color[0] >> c >> color[1] >> c >> color[2];
        return color;
    }

    /* Declarative field table
    - Source for every "simple" config field (as in anything that is one key = one value)
    - Previously this lived as separate hand-written lists that had to be kept in sync by hand.
      CHART_COLORS is the one field that can't fit in this table, since parsing it consumes multiple lines from the stream;
      It's still handled as a special case in parseConfig().
    */
    static int parseValue(int*, const std::string& v) {
        return std::stoi(v);
    }

    static double parseValue(double*, const std::string& v) {
        return std::stod(v);
    }

    static bool parseValue(bool*, const std::string& v) {
        return v == "true";
    }

    static std::string parseValue(std::string*, const std::string& v) {
        return unquote(v);
    }

    static std::array<int, 3> parseValue(std::array<int, 3>*, const std::string& v) {
        return parseColor(v);
    }

    using Setter = std::function<void(Config&, const std::string&)>;

    struct FieldSpec {
        const char* key;
        Setter setter;
        bool passthrough;
    };

    /* Builds a Setter for `member` with zero repetition of its type.
    - T is deduced from the member pointer.
    - parseValue() is picked by overload resolution on that same T.
    */
    template <typename T> static Setter makeSetter(T Config::* member) {
        return [member](Config& c, const std::string& v) {
            c.*member = parseValue(static_cast<T*>(nullptr), v);
        };
    }

    static const std::vector<FieldSpec>& fieldTable() {
        static const std::vector<FieldSpec> table = {
            {"SIDEBAR_WIDTH",                       makeSetter(&Config::SIDEBAR_WIDTH),                       false},
            {"SIDEBAR_SELECTED_FOLDER_ICON",        makeSetter(&Config::SIDEBAR_SELECTED_FOLDER_ICON),        true},
            {"SIDEBAR_SELECTED_FILE_ICON",          makeSetter(&Config::SIDEBAR_SELECTED_FILE_ICON),          true},
            {"SIDEBAR_FOLDER_ICON",                 makeSetter(&Config::SIDEBAR_FOLDER_ICON),                 true},
            {"SIDEBAR_FILE_ICON",                   makeSetter(&Config::SIDEBAR_FILE_ICON),                   true},
            {"SIDEBAR_BACK_ICON",                   makeSetter(&Config::SIDEBAR_BACK_ICON),                   true},
            {"SIDEBAR_GROWTH_COLOR",                makeSetter(&Config::SIDEBAR_GROWTH_COLOR),                true},
            {"SIDEBAR_SHRINK_COLOR",                makeSetter(&Config::SIDEBAR_SHRINK_COLOR),                true},
            {"SIDEBAR_GROWTH_THRESHOLD_PERCENTAGE", makeSetter(&Config::SIDEBAR_GROWTH_THRESHOLD_PERCENTAGE), true},
            {"CHART_MAX_SIZE_THRESHOLD_PERCENTAGE", makeSetter(&Config::CHART_MAX_SIZE_THRESHOLD_PERCENTAGE), true},
            {"CHART_MAX_GENERATIONS",               makeSetter(&Config::CHART_MAX_GENERATIONS),               true},
            {"CHART_MIN_SLICES",                    makeSetter(&Config::CHART_MIN_SLICES),                    true},
            {"CHART_DIM_FACTOR",                    makeSetter(&Config::CHART_DIM_FACTOR),                    true},
            {"CHART_CELL_RATIO",                    makeSetter(&Config::CHART_CELL_RATIO),                    true},
            {"CHART_HIGHLIGHT_SUBTREE",             makeSetter(&Config::CHART_HIGHLIGHT_SUBTREE),             true},
            {"ENABLE_FOLDER_SIZE_PERCENTAGES",      makeSetter(&Config::ENABLE_FOLDER_SIZE_PERCENTAGES),      true},
            {"SCANNER_THREADS",                     makeSetter(&Config::SCANNER_THREADS),                     true},
            {"COLOR_MODE",                          makeSetter(&Config::COLOR_MODE),                          true},
            {"CLEANUP_OLD_DAYS",                    makeSetter(&Config::CLEANUP_OLD_DAYS),                    true},
        };
        return table;
    }

    static const FieldSpec* findField(const std::string& key) {
        for (const auto& field : fieldTable()) {
            if (key == field.key) return &field;
        }
        return nullptr;
    }

    static std::vector<std::array<int, 3>> parseColorList(const std::string& firstLineValue, std::istream& in) {
        std::string block;

        if (firstLineValue.find('[') != std::string::npos) {
            block += firstLineValue.substr(firstLineValue.find('[') + 1);
            std::string line;

            while (std::getline(in, line)) {
                if (line.find(']') != std::string::npos) {
                    block += line.substr(0, line.find(']'));
                    break;
                }
                block += line;
            }
        }

        std::vector<std::array<int, 3>> colors;
        std::stringstream ss(block);
        std::string tuple;

        while (std::getline(ss, tuple, '}')) {
            size_t start = tuple.find('{');
            if (start == std::string::npos) continue;

            colors.push_back(parseColor(tuple.substr(start)));
        }

        return colors;
    }

    /* Reads a config file and populates a Config object:
    - Starts from `cfg`: keys the file doesn't have keep its values
    - Ignores empty lines and comments (#)
    - Splits lines by '=' into key/value
    - Trims leading/trailing spaces and tabs
    - Converts values to int, double, or string as appropriate
    - Special handling for CHART_COLORS array block
    */
    static std::optional<Config> parseConfig(std::istream& in) {
        return parseConfig(in, Config());
    }

    static std::optional<Config> parseConfig(std::istream& in, Config cfg) {
        std::string line;

        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // A comment after the value: "CHART_HIGHLIGHT_SUBTREE = true  # ..." was read as false
            if (size_t comment = commentStart(value); comment != std::string::npos)
                value.erase(comment);

            trim(key);
            trim(value);

            try {
                if (key == "CHART_COLORS") {
                    // Multi-line array block; doesn't fit the single-value
                    // field table, so it's parsed separately.
                    cfg.CHART_COLORS = parseColorList(value, in);
                } else if (const FieldSpec* field = findField(key)) {
                    field->setter(cfg, isLegacyIconValue(value) ? defaultValue(key) : value);
                }
                // Unknown keys are silently ignored, same as before.
            } catch (...) {
                // A value that doesn't parse ("SIDEBAR_WIDTH = abc"): that key keeps its default, the others are read
                continue;
            }
        }

        return cfg;
    }
};
