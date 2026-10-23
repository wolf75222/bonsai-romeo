#pragma once

#include <sstream>
#include <iomanip>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <ftxui/screen/string.hpp>

class FormatUtils {
public:
    // Just convert a size in bytes to a human readable format
    static std::string toReadable(uintmax_t bytes, const std::string& spacer = " ") {
        constexpr uintmax_t KB = 1024;
        constexpr uintmax_t MB = KB * 1024;
        constexpr uintmax_t GB = MB * 1024;

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);

        if (bytes >= GB)
            oss << (double)bytes / GB << spacer << "GiB";
        else if (bytes >= MB)
            oss << (double)bytes / MB << spacer << "MiB";
        else if (bytes >= KB)
            oss << (double)bytes / KB << spacer << "KiB";
        else
            oss << bytes << spacer << "B";

        return oss.str();
    }

    // Same thing without the spacer argument
    static std::string toReadableShort(uintmax_t bytes) {
        return toReadable(bytes, "");
    }

    /* Size for narrow columns, never more than 5 characters: 512B, 64.0K, 911M, 2.61G, 28.1G
    - "28.07GiB" didn't fit the sidebar column and was cut to "28.07Gi"
    */
    static std::string toCompact(uintmax_t bytes) {
        static const char* units[] = {"B", "K", "M", "G", "T", "P"};

        if (bytes < 1000)
            return std::to_string(bytes) + "B";

        double value = static_cast<double>(bytes);
        int unit = 0;
        while (value >= 999.5 && unit < 5) {
            value /= 1024.0;
            unit++;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(value < 9.995 ? 2 : (value < 99.95 ? 1 : 0)) << value << units[unit];
        return oss.str();
    }

    // A duration in a short format: 5 min, 3 h, 12 d, 7 mo, 1.4 y
    static std::string toAge(int64_t seconds) {
        if (seconds < 0) seconds = 0;

        std::ostringstream oss;
        if (seconds < 3600)
            oss << seconds / 60 << " min";
        else if (seconds < 86400)
            oss << seconds / 3600 << " h";
        else if (seconds < 60 * 86400)
            oss << seconds / 86400 << " d";
        else if (seconds < 365 * 86400)
            oss << seconds / (30 * 86400) << " mo";
        else
            oss << std::fixed << std::setprecision(1) << seconds / (365.0 * 86400) << " y";

        return oss.str();
    }

    // A count in a short format: 987, 12.3k, 1.50M
    static std::string toCount(uintmax_t count, const std::string& spacer = "") {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(count >= 1'000'000 ? 2 : 1);

        if (count >= 1'000'000'000)
            oss << (double)count / 1e9 << spacer << "G";
        else if (count >= 1'000'000)
            oss << (double)count / 1e6 << spacer << "M";
        else if (count >= 1'000)
            oss << (double)count / 1e3 << spacer << "k";
        else
            oss << count;

        return oss.str();
    }

    // A count with thousands separators: 1 496 302
    static std::string withSeparators(uintmax_t count) {
        std::string digits = std::to_string(count);
        std::string out;

        for (size_t i = 0; i < digits.size(); i++) {
            if (i > 0 && (digits.size() - i) % 3 == 0)
                out += ' ';
            out += digits[i];
        }

        return out;
    }

    // Number of characters (code points) in an UTF-8 string
    static size_t length(const std::string& str) {
        size_t count = 0;
        for (unsigned char c : str)
            if ((c & 0xC0) != 0x80) count++;
        return count;
    }

    // Byte offset of the character number `chars`, never in the middle of a multi-byte character
    static size_t offsetOf(const std::string& str, size_t chars) {
        size_t count = 0;
        for (size_t i = 0; i < str.size(); i++) {
            if ((static_cast<unsigned char>(str[i]) & 0xC0) != 0x80) {
                if (count == chars) return i;
                count++;
            }
        }
        return str.size();
    }

    // Width on screen, in columns: CJK characters and most emoji take two (FTXUI's own measure)
    static size_t width(const std::string& str) {
        return static_cast<size_t>(std::max(0, ftxui::string_width(str)));
    }

    // Byte length of the UTF-8 character starting at `str[i]`
    static size_t charLength(const std::string& str, size_t i) {
        const unsigned char c = static_cast<unsigned char>(str[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        return std::min(len, str.size() - i);
    }

    // The longest start of `str` (whole characters) at most `cols` wide
    static std::string fitStart(const std::string& str, size_t cols) {
        std::string out;
        size_t used = 0;
        for (size_t i = 0; i < str.size();) {
            const size_t len = charLength(str, i);
            const std::string ch = str.substr(i, len);
            const size_t w = width(ch);
            if (used + w > cols) break;
            out += ch;
            used += w;
            i += len;
        }
        return out;
    }

    // The longest end of `str` (whole characters) at most `cols` wide
    static std::string fitEnd(const std::string& str, size_t cols) {
        std::vector<std::string> chars;
        for (size_t i = 0; i < str.size(); i += charLength(str, i))
            chars.push_back(str.substr(i, charLength(str, i)));

        std::string out;
        size_t used = 0;
        for (size_t k = chars.size(); k-- > 0;) {
            const size_t w = width(chars[k]);
            if (used + w > cols) break;
            out = chars[k] + out;
            used += w;
        }
        return out;
    }

    // Keeps the start and the end (the file name) of a long path: /project/r1/.../releases/full-4bc/f.sqsh
    static std::string trimMiddle(const std::string& str, size_t max_size) {
        if (width(str) <= max_size)
            return str;
        if (max_size < 8)
            return trimSuffix(str, max_size, "...");

        const size_t keep_end = (max_size - 3) * 2 / 3;
        const size_t keep_start = max_size - 3 - keep_end;
        return fitStart(str, keep_start) + "..." + fitEnd(str, keep_end);
    }

    // Cuts to `max_size` columns (not bytes, not characters: "数" takes two), ending with `suffix`
    static std::string trimSuffix(const std::string& str, size_t max_size, const std::string& suffix) {
        if (width(str) <= max_size)
            return str;

        const size_t suffix_width = width(suffix);
        if (suffix_width >= max_size)
            return fitStart(suffix, max_size);

        return fitStart(str, max_size - suffix_width) + suffix;
    }
};
