#pragma once

#include <sstream>
#include <iomanip>
#include <cstdint>
#include <string>

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

    static std::string trimSuffix(const std::string& str, size_t max_size, const std::string& suffix) {
        if (str.size() <= max_size)
            return str;

        if (suffix.size() >= max_size)
            return suffix.substr(0, max_size);

        size_t cut = max_size - suffix.size();
        return str.substr(0, cut) + suffix;
    }
};
