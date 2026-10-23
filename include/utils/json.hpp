/* JSON
- The little --json needs: strings, quoted and escaped.
- File names can hold any byte: what isn't UTF-8 becomes U+FFFD, so the output is always valid JSON.
*/

#pragma once

#include <cstdio>
#include <string>

namespace Json {

inline std::string str(const std::string& s) {
    std::string out = "\"";

    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);

        if (c < 0x80) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                default:
                    if (c < 0x20) {
                        char escaped[8];
                        std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                        out += escaped;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
            i++;
            continue;
        }

        // A UTF-8 sequence: its lead byte gives its length, the others must be continuation bytes
        const size_t length = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        bool valid = length > 0 && i + length <= s.size();
        for (size_t k = 1; valid && k < length; k++)
            valid = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;

        if (valid) {
            out.append(s, i, length);
            i += length;
        } else {
            out += "\\ufffd";
            i++;
        }
    }

    return out + "\"";
}

}
